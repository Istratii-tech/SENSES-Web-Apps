/*
 * Copyright (C) 2011 The Android Open Source Project
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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_DISPLAY_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_DISPLAY_IMPL_H_

#include <EGL/egl.h>
#include <set>

#include "graphics_translation/egl/color_buffer.h"
#include "graphics_translation/egl/egl_context_impl.h"
#include "graphics_translation/egl/egl_surface_impl.h"
#include "graphics_translation/egl/object_registry.h"
#include "graphics_translation/gles/cond.h"
#include "graphics_translation/gles/mutex.h"

class EglConfigImpl;
struct NativeConfig;
struct NativeWindow;

class EglDisplayImpl {
 public:
  typedef ObjectRegistry<ContextPtr> ContextRegistry;
  typedef ObjectRegistry<SurfacePtr> SurfaceRegistry;
  typedef ObjectRegistry<ColorBufferPtr> ColorBufferRegistry;

  static const EGLDisplay kDefaultDisplay;

  static EglDisplayImpl* GetDefaultDisplay();
  static EglDisplayImpl* GetDisplay(EGLDisplay dpy);

  EglDisplayImpl();
  ~EglDisplayImpl();

  bool IsInitialized() const { return initialized_; }

  ContextPtr GetGlobalContext() { return contexts_.Get(global_context_); }

  ContextRegistry& GetContexts() { return contexts_; }
  SurfaceRegistry& GetSurfaces() { return surfaces_; }
  ColorBufferRegistry& GetColorBuffers() { return color_buffers_; }

  void Acquire();
  void Release();

  // Save the current context and switch to the global context.
  bool Lock();

  // Draw the specified texture as a full-screen image.
  void DrawFullscreenQuadLocked(GLuint texture, bool flip_v);

  // Swap the main NativeWindow object.  Must be called after LockWindow().
  void SwapBuffersLocked();

  void OnGraphicsContextsLost();
  void OnGraphicsContextsRestored();

  void OnColorBufferAcquiredLocked();
  void OnColorBufferReleasedLocked();

  bool IsValidLocked() const { return !invalidated_; }

  // Restore the saved context.
  bool Unlock();

  // Helper functions for changing the current thread context.
  EGLint MakeCurrent(EGLContext context, EGLSurface draw, EGLSurface read);
  EGLint SwapBuffers(EGLSurface srfc);

  // Functions for querying and selecting configs.
  int GetNumConfigs() const { return configs_.size(); }
  EGLBoolean GetConfigs(EGLConfig* configs, EGLint config_size,
                        EGLint* num_config);
  EGLBoolean ChooseConfigs(const EGLint* attribs, EGLConfig* configs,
                           EGLint configs_size, EGLint* num_config);

  bool IsValidConfig(EGLConfig config) const;
  EGLBoolean GetConfigAttribute(EGLConfig config, EGLint attrib, EGLint* value);
  EGLBoolean GetConfigPixelFormat(EGLConfig config, EGLenum* format,
                                  EGLenum* type);
  EGLBoolean AreConfigsCompatible(EGLConfig lhs, EGLConfig rhs) const;

  const EglConfigImpl* GetConfig(EGLConfig id) const;

 private:
  typedef std::set<EglConfigImpl> ConfigSet;

  static void CreateDefaultDisplay();

  void Initialize();
  void Shutdown();
  void BindLocked();
  void UnbindLocked();

  static EglDisplayImpl* default_display_;

  Mutex lock_;
  Cond cond_no_locked_buffers_;
  bool initialized_;
  bool invalidated_;

  // EGL objects.
  ConfigSet configs_;
  ContextRegistry contexts_;
  SurfaceRegistry surfaces_;
  ColorBufferRegistry color_buffers_;
  int color_buffers_locked_;

  // The global context will be used for the main window.  It is also shared
  // with all others contexts that are created.
  EGLContext global_context_;

  // Native handles to the main window.
  NativeWindow* window_;

  EglDisplayImpl(const EglDisplayImpl& rhs);
  EglDisplayImpl& operator=(const EglDisplayImpl& rhs);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_DISPLAY_IMPL_H_
