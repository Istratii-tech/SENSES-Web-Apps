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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_THREAD_INFO_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_THREAD_INFO_H_

#include <EGL/egl.h>

#include "graphics_translation/egl/egl_context_impl.h"
#include "graphics_translation/egl/egl_surface_impl.h"

class GlesContext;

// This class stores the thread-specific EGL context and error values.
class EglThreadInfo {
 public:
  static EglThreadInfo& GetInstance();

  void SetError(EGLint error);
  EGLint GetError() const;

  ContextPtr GetCurrentContext();
  void SetCurrentContext(ContextPtr ctx);

  // Saves the current context (so that it can be temporarily switched to
  // another context) such that it can be restored later.
  void SaveCurrentContext();

  // Restores the previously saved context.
  void RestorePreviousContext();

  bool SetReportedNoContextError();

  // During destroying a GlesContext, a destroying GlesContext will be set.
  // All PASS_THROUGH used by destructors should use this context instead of
  // the default current context.
  void SetDestroyingGlesContext(GlesContext* context);
  GlesContext* GetDestroyingGlesContext();

 private:
  EGLint error_;
  ContextPtr curr_ctx_;
  ContextPtr prev_ctx_;
  bool reported_no_context_error_;
  GlesContext *destroying_gles_context_;

  // Cannot instantiate this class directly.  Instead, users must call the
  // GetInstance() function.
  EglThreadInfo();

  EglThreadInfo(const EglThreadInfo&);
  EglThreadInfo& operator=(const EglThreadInfo&);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_THREAD_INFO_H_
