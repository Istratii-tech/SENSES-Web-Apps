// GLES 1.1 demo for ARC's graphics_translation running on WebGL2.
//
// The rendering path is the one Android apps took under ARC:
//
//   app (GLES 1.1, EGL)                         <- this file
//     -> graphics_translation/gles             fixed-function -> GLES2 shaders
//     -> graphics_translation/egl              EGL window surface
//     -> FramebufferWindow (ANativeWindow)     buffers from the gralloc HAL
//     -> gralloc FB0 post -> ColorBuffer::Render  composite to the display
//     -> native_webgl.cpp                      virtual contexts on WebGL2
//     -> <canvas>
//
// FramebufferWindow plays the role of Android's FramebufferNativeWindow:
// two gralloc buffers, rendered into in turn and posted to the framebuffer
// device on eglSwapBuffers.
//
// Files come through ARC's posix_translation: /system is a read-only image
// (tools/make_system_image.py) mounted in its VirtualFileSystem, and the app
// reads it with ordinary fopen()/fread() -- shim/kernel routes libc there.
// /data is writable storage in the browser's OPFS (a launch counter shows
// that it survives reloads).
//
// main() runs on a pthread (PROXY_TO_PTHREAD); the canvas is transferred to
// it as an OffscreenCanvas, so GL, the VFS and the app share one thread.

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <vector>

#include "common/options.h"
#include "graphics_translation/gralloc/gralloc.h"
#include "kernel/arc_kernel.h"
#include "posix_translation/host_file.h"
#include "posix_translation/opfs_file.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/virtual_file_system.h"
#include "hardware/fb.h"
#include "hardware/gralloc.h"
#include "system/window.h"

// Defined by graphics_translation/gralloc/gralloc_main.cpp.
struct private_module_t {
  gralloc_module_t base;
};
extern private_module_t HAL_MODULE_INFO_SYM;

namespace {

// --- ANativeWindow over the gralloc HAL -------------------------------------

class FramebufferWindow : public ANativeWindow {
 public:
  static const int kNumBuffers = 2;

  FramebufferWindow() : refs_(1), next_(0) {
    common.incRef = &IncRef;
    common.decRef = &DecRef;
    setSwapInterval = &SetSwapInterval;
    dequeueBuffer_DEPRECATED = &DequeueBufferDeprecated;
    lockBuffer_DEPRECATED = &LockBufferDeprecated;
    queueBuffer_DEPRECATED = &QueueBufferDeprecated;
    query = &Query;
    perform = &Perform;
    cancelBuffer_DEPRECATED = &CancelBufferDeprecated;
    dequeueBuffer = &DequeueBuffer;
    queueBuffer = &QueueBuffer;
    cancelBuffer = &CancelBuffer;
    const_cast<int&>(minSwapInterval) = 1;
    const_cast<int&>(maxSwapInterval) = 1;
  }

  // Must run after eglInitialize(): gralloc buffers own EGL color buffers.
  bool Init() {
    hw_module_t* module = &HAL_MODULE_INFO_SYM.base.common;
    if (module->methods->open(module, GRALLOC_HARDWARE_GPU0,
                              reinterpret_cast<hw_device_t**>(&gpu_)) != 0 ||
        module->methods->open(module, GRALLOC_HARDWARE_FB0,
                              reinterpret_cast<hw_device_t**>(&fb_)) != 0) {
      return false;
    }
    width_ = fb_->width;
    height_ = fb_->height;
    const_cast<float&>(xdpi) = fb_->xdpi;
    const_cast<float&>(ydpi) = fb_->ydpi;
    const int usage = GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_HW_RENDER |
                      GRALLOC_USAGE_HW_TEXTURE;
    for (int i = 0; i < kNumBuffers; ++i) {
      ANativeWindowBuffer* b = &buffers_[i];
      buffer_handle_t handle = NULL;
      int stride = 0;
      if (gpu_->alloc(gpu_, width_, height_, HAL_PIXEL_FORMAT_RGBA_8888,
                      usage, &handle, &stride) != 0) {
        return false;
      }
      b->width = width_;
      b->height = height_;
      b->stride = stride;
      b->format = HAL_PIXEL_FORMAT_RGBA_8888;
      b->usage = usage;
      b->handle = handle;
    }
    return true;
  }

 private:
  static FramebufferWindow* Self(ANativeWindow* w) {
    return static_cast<FramebufferWindow*>(w);
  }
  static FramebufferWindow* Self(const ANativeWindow* w) {
    return const_cast<FramebufferWindow*>(
        static_cast<const FramebufferWindow*>(w));
  }

  static void IncRef(android_native_base_t* base) {
    ++reinterpret_cast<FramebufferWindow*>(base)->refs_;
  }
  static void DecRef(android_native_base_t* base) {
    --reinterpret_cast<FramebufferWindow*>(base)->refs_;  // Never freed.
  }
  static int SetSwapInterval(ANativeWindow* w, int interval) {
    return Self(w)->fb_->setSwapInterval(Self(w)->fb_, interval);
  }
  static int DequeueBuffer(ANativeWindow* w, ANativeWindowBuffer** buffer,
                           int* fence_fd) {
    FramebufferWindow* self = Self(w);
    *buffer = &self->buffers_[self->next_];
    self->next_ = (self->next_ + 1) % kNumBuffers;
    if (fence_fd)
      *fence_fd = -1;  // Already available: no fence.
    return 0;
  }
  static int QueueBuffer(ANativeWindow* w, ANativeWindowBuffer* buffer,
                         int fence_fd) {
    // Post = composite this buffer onto the display (the canvas).
    return Self(w)->fb_->post(Self(w)->fb_, buffer->handle);
  }
  static int CancelBuffer(ANativeWindow* w, ANativeWindowBuffer* buffer,
                          int fence_fd) {
    return 0;
  }
  static int DequeueBufferDeprecated(ANativeWindow* w,
                                     ANativeWindowBuffer** buffer) {
    return DequeueBuffer(w, buffer, NULL);
  }
  static int LockBufferDeprecated(ANativeWindow* w,
                                  ANativeWindowBuffer* buffer) {
    return 0;
  }
  static int QueueBufferDeprecated(ANativeWindow* w,
                                   ANativeWindowBuffer* buffer) {
    return QueueBuffer(w, buffer, -1);
  }
  static int CancelBufferDeprecated(ANativeWindow* w,
                                    ANativeWindowBuffer* buffer) {
    return 0;
  }
  static int Query(const ANativeWindow* w, int what, int* value) {
    FramebufferWindow* self = Self(w);
    switch (what) {
      case NATIVE_WINDOW_WIDTH:
      case NATIVE_WINDOW_DEFAULT_WIDTH:
        *value = self->width_;
        return 0;
      case NATIVE_WINDOW_HEIGHT:
      case NATIVE_WINDOW_DEFAULT_HEIGHT:
        *value = self->height_;
        return 0;
      case NATIVE_WINDOW_FORMAT:
        *value = HAL_PIXEL_FORMAT_RGBA_8888;
        return 0;
      case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
        *value = 1;
        return 0;
      case NATIVE_WINDOW_CONCRETE_TYPE:
        *value = NATIVE_WINDOW_FRAMEBUFFER;
        return 0;
      case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER:
        *value = 0;
        return 0;
      case NATIVE_WINDOW_TRANSFORM_HINT:
        *value = 0;
        return 0;
    }
    return -EINVAL;
  }
  static int Perform(ANativeWindow* w, int operation, ...) {
    // Connect/disconnect, format, timestamps, crop...: the framebuffer has a
    // fixed configuration, so accept and ignore them.
    return 0;
  }

  int refs_;
  int width_;
  int height_;
  int next_;
  alloc_device_t* gpu_;
  framebuffer_device_t* fb_;
  ANativeWindowBuffer buffers_[kNumBuffers];
};

// --- Files: libc -> ARC's VirtualFileSystem (shim/kernel) -----------------

void InitFileSystem() {
  posix_translation::VirtualFileSystem* vfs = arc_kernel::Boot();
  // The image lives in the wasm host's file system (embedded at link time);
  // ReadonlyFileHandler mmaps it and serves the files inside.
  vfs->Mount("/system/", new posix_translation::ReadonlyFileHandler(
      "/arc/system.img", 64 * 1024,
      new posix_translation::HostFileHandler()));
  // Writable, persistent app storage in the browser's OPFS.
  vfs->Mount("/data/", new posix_translation::OpfsHandler);
}

// Counts launches in /data, which survives page reloads.
int CountLaunch() {
  const char kDir[] = "/data/arc_demo";
  const char kFile[] = "/data/arc_demo/launches";
  if (mkdir(kDir, 0755) != 0 && errno != EEXIST) {
    printf("mkdir(%s) failed: %s\n", kDir, strerror(errno));
    return -1;
  }
  int launches = 0;
  if (FILE* f = fopen(kFile, "r")) {
    if (fscanf(f, "%d", &launches) != 1)
      launches = 0;
    fclose(f);
  }
  ++launches;
  FILE* f = fopen(kFile, "w");
  if (!f) {
    printf("fopen(%s) failed: %s\n", kFile, strerror(errno));
    return -1;
  }
  fprintf(f, "%d\n", launches);
  fclose(f);
  return launches;
}

// Plain stdio from here on: the kernel layer routes it into the VFS.
ssize_t ReadFile(const char* path, void* buf, size_t size) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("fopen(%s) failed: %s\n", path, strerror(errno));
    return -1;
  }
  const size_t n = fread(buf, 1, size, f);
  fclose(f);
  return n;
}

// --- The GLES 1.1 "app" ---------------------------------------------------

EGLDisplay g_display;
EGLSurface g_surface;
EGLContext g_context;
GLuint g_texture;
int g_width;
int g_height;
int g_frame = 0;

// A unit cube: 6 faces x 4 vertices, with normals, colors and UVs.
GLfloat g_positions[24 * 3];
GLfloat g_normals[24 * 3];
GLubyte g_colors[24 * 4];
GLubyte g_white[24 * 4];
GLfloat g_uvs[24 * 2];
GLushort g_indices[36];

void BuildCube() {
  static const GLfloat kFaceNormals[6][3] = {
    {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  static const GLubyte kFaceColors[6][3] = {
    {255, 99, 71}, {65, 105, 225}, {60, 179, 113},
    {255, 215, 0}, {186, 85, 211}, {0, 206, 209}};
  static const GLfloat kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  // Per face, axes u and v with u x v = normal, so the corners above wind
  // counter-clockwise seen from outside (GL_CULL_FACE keeps them).
  static const GLfloat kFaceU[6][3] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
  static const GLfloat kFaceV[6][3] = {
    {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  for (int f = 0; f < 6; ++f) {
    const GLfloat* n = kFaceNormals[f];
    const GLfloat* u = kFaceU[f];
    const GLfloat* v = kFaceV[f];
    for (int c = 0; c < 4; ++c) {
      const int i = f * 4 + c;
      for (int k = 0; k < 3; ++k) {
        g_positions[i * 3 + k] =
            n[k] + kCorners[c][0] * u[k] + kCorners[c][1] * v[k];
        g_normals[i * 3 + k] = n[k];
      }
      for (int k = 0; k < 3; ++k)
        g_colors[i * 4 + k] = kFaceColors[f][k];
      g_colors[i * 4 + 3] = 255;
      g_uvs[i * 2 + 0] = (kCorners[c][0] + 1) / 2;
      // Image rows run top to bottom; GL texture t=0 is the first row.
      g_uvs[i * 2 + 1] = 1 - (kCorners[c][1] + 1) / 2;
    }
    const GLushort b = f * 4;
    const GLushort tri[6] = {b, (GLushort)(b + 1), (GLushort)(b + 2),
                             b, (GLushort)(b + 2), (GLushort)(b + 3)};
    memcpy(&g_indices[f * 6], tri, sizeof(tri));
  }
}

bool BuildTexture() {
  // 128x128 RGBA, read from the Android /system image via the VFS.
  static GLubyte pixels[128 * 128 * 4];
  const ssize_t n = ReadFile("/system/media/arc_demo_texture.rgba", pixels,
                             sizeof(pixels));
  if (n != static_cast<ssize_t>(sizeof(pixels))) {
    printf("texture: read %zd of %zu bytes\n", n, sizeof(pixels));
    return false;
  }
  printf("Texture: /system/media/arc_demo_texture.rgba (%zd bytes, fread via VFS)\n",
         n);
  glGenTextures(1, &g_texture);
  glBindTexture(GL_TEXTURE_2D, g_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  return true;
}

void DrawCube(const GLubyte* colors) {
  glVertexPointer(3, GL_FLOAT, 0, g_positions);
  glNormalPointer(GL_FLOAT, 0, g_normals);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
  glTexCoordPointer(2, GL_FLOAT, 0, g_uvs);
  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, g_indices);
}

// Reads the frame back and prints a summary, so a browser that cannot be
// screenshotted (or a headless run) still shows whether drawing works.
void SelfCheck() {
  std::vector<uint8_t> px(static_cast<size_t>(g_width) * g_height * 4);
  glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  // Clear color (0.07, 0.08, 0.12) in 8 bits, give or take rounding.
  size_t drawn = 0;
  std::set<uint32_t> colors;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (abs(px[i] - 18) > 2 || abs(px[i + 1] - 20) > 2 || abs(px[i + 2] - 31) > 2)
      ++drawn;
    colors.insert((px[i] >> 4) << 8 | (px[i + 1] >> 4) << 4 | px[i + 2] >> 4);
  }
  const uint8_t* c = &px[(static_cast<size_t>(g_height / 2) * g_width + g_width / 2) * 4];
  printf("Self-check at frame 120: %.1f%% of pixels drawn, %zu colors, "
         "center rgb(%d,%d,%d), glGetError=0x%x\n",
         100.0 * drawn / (px.size() / 4), colors.size(), c[0], c[1], c[2],
         glGetError());
}

void Frame() {
  const float t = g_frame++ / 60.0f;

  glViewport(0, 0, g_width, g_height);
  glClearColor(0.07f, 0.08f, 0.12f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  const float aspect = static_cast<float>(g_width) / g_height;
  glFrustumf(-aspect * 0.5f, aspect * 0.5f, -0.5f, 0.5f, 1.0f, 50.0f);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(0, 0, -7);

  // Light fixed in eye space, upper left.
  const GLfloat light_pos[4] = {-3, 4, 4, 1};
  glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

  // Big cube, textured.
  glPushMatrix();
  glRotatef(t * 40, 0, 1, 0);
  glRotatef(t * 23, 1, 0, 0);
  glEnable(GL_TEXTURE_2D);
  DrawCube(g_white);
  glPopMatrix();

  // Small untextured cube on an orbit.
  glPushMatrix();
  glRotatef(t * 90, 0, 0, 1);
  glTranslatef(2.6f, 0, 0);
  glRotatef(t * 180, 1, 1, 0);
  glScalef(0.35f, 0.35f, 0.35f);
  glDisable(GL_TEXTURE_2D);
  DrawCube(g_colors);
  glPopMatrix();

  if (g_frame == 120)
    SelfCheck();
  eglSwapBuffers(g_display, g_surface);
}

bool Init() {
  InitFileSystem();
  printf("Launch #%d (counter kept in /data/arc_demo/launches on OPFS)\n",
         CountLaunch());
  char build_prop[256] = {0};
  if (ReadFile("/system/build.prop", build_prop, sizeof(build_prop) - 1) > 0)
    printf("/system/build.prop:\n%s", build_prop);

  arc::Options* options = arc::Options::GetInstance();
  options->Put("enable_compositor", "false");
  options->Put("enable_gl_error_check", "false");
  options->Put("disable_gl_fixed_attribs", "false");

  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(g_display, NULL, NULL)) {
    printf("eglInitialize failed\n");
    return false;
  }
  static const EGLint kConfigAttribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 16,
    EGL_NONE,
  };
  EGLConfig config;
  EGLint num_configs = 0;
  if (!eglChooseConfig(g_display, kConfigAttribs, &config, 1, &num_configs) ||
      num_configs < 1) {
    printf("eglChooseConfig failed\n");
    return false;
  }

  static FramebufferWindow window;
  if (!window.Init()) {
    printf("gralloc framebuffer window setup failed\n");
    return false;
  }
  g_surface = eglCreateWindowSurface(g_display, config, &window, NULL);
  static const EGLint kContextAttribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT,
                               kContextAttribs);
  if (g_surface == EGL_NO_SURFACE || g_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
    printf("EGL surface/context setup failed: 0x%x\n", eglGetError());
    return false;
  }
  eglQuerySurface(g_display, g_surface, EGL_WIDTH, &g_width);
  eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &g_height);

  printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n",
         glGetString(GL_VENDOR), glGetString(GL_RENDERER),
         glGetString(GL_VERSION));
  printf("libc calls routed into the VFS so far: %lu\n",
         arc_kernel::RoutedCallCount());
  printf("Surface: %dx%d, GL thread is %s\n", g_width, g_height,
         emscripten_is_main_browser_thread() ? "the main browser thread"
                                             : "a worker (OffscreenCanvas)");

  BuildCube();
  memset(g_white, 255, sizeof(g_white));
  if (!BuildTexture())
    return false;
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glShadeModel(GL_SMOOTH);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_COLOR_MATERIAL);
  glEnable(GL_NORMALIZE);
  const GLfloat ambient[4] = {0.25f, 0.25f, 0.3f, 1};
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_NORMAL_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  return true;
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE int arc_demo_frame_count() { return g_frame; }

int main() {
  if (!Init())
    return 1;
  emscripten_set_main_loop(Frame, 0, 0);
  return 0;
}
