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
#include "graphics_translation/egl/egl_display_impl.h"

#include <stdlib.h>
#include <algorithm>
#include "common/alog.h"
#include "graphics_translation/egl/egl_config_impl.h"
#include "graphics_translation/egl/egl_pbuffer_surface_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/egl/native.h"
#include "graphics_translation/gles/gles_context.h"

static pthread_once_t default_display_once_init = PTHREAD_ONCE_INIT;

EglDisplayImpl* EglDisplayImpl::default_display_ = NULL;
const EGLDisplay EglDisplayImpl::kDefaultDisplay = (EGLDisplay)1;

void EglDisplayImpl::CreateDefaultDisplay() {
  LOG_ALWAYS_FATAL_IF(default_display_ != NULL);
  default_display_ = new EglDisplayImpl();
}

EglDisplayImpl* EglDisplayImpl::GetDefaultDisplay() {
  pthread_once(&default_display_once_init, CreateDefaultDisplay);
  return default_display_;
}

EglDisplayImpl* EglDisplayImpl::GetDisplay(EGLDisplay dpy) {
  return dpy == kDefaultDisplay ? GetDefaultDisplay() : NULL;
}

EglDisplayImpl::EglDisplayImpl()
  : initialized_(false),
    invalidated_(false),
    color_buffers_locked_(0),
    global_context_(NULL),
    window_(NULL) {
  ConfigsList configs;
  Native::QueryConfigs(&configs);

  // Start at 1 since 0 is EGL_NO_CONFIG.
  int id = 1;
  for (ConfigsList::iterator it = configs.begin(); it != configs.end(); ++it) {
    configs_.insert(EglConfigImpl(id, *it));
    ++id;
  }
}

EglDisplayImpl::~EglDisplayImpl() {
  Shutdown();
}

void EglDisplayImpl::Initialize() {
  Mutex::Autolock mutex(&lock_);
  if (initialized_) {
    return;
  }

  window_ = Native::CreateNativeWindow();
  LOG_ALWAYS_FATAL_IF(!window_, "Could not create native window.");

  EGLConfig cfg = NULL;
  for (ConfigSet::iterator it = configs_.begin(); it != configs_.end(); ++it) {
    const EGLint r = it->GetValue(EGL_RED_SIZE);
    const EGLint g = it->GetValue(EGL_GREEN_SIZE);
    const EGLint b = it->GetValue(EGL_BLUE_SIZE);
    if (r > 0 && g > 0 && b > 0) {
      cfg = it->GetKey();
      break;
    }
  }

  EGLint err = 0;
  global_context_ = EglContextImpl::Create(kDefaultDisplay, cfg, NULL, 2, &err);

  // Bind the window surface here in order for the compositor to be associated
  // with the correct context.  (The compositor associates itself to the first
  // surface that is bound.)
  ContextPtr ctx = contexts_.Get(global_context_);
  Native::BindNativeWindow(window_, ctx->GetNativeContext());

  // Force the GlesContext owned by the global context to be initialized at
  // least once.
  EglThreadInfo& info = EglThreadInfo::GetInstance();
  info.SetCurrentContext(ctx);
  ctx->GetGlesContext()->OnMakeCurrent();
  info.SetCurrentContext(ContextPtr());

  initialized_ = true;
}

void EglDisplayImpl::Shutdown() {
  Mutex::Autolock mutex(&lock_);
  if (!initialized_) {
    return;
  }

  MakeCurrent(EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE);
  contexts_.Unregister(global_context_);
  global_context_ = NULL;
  Native::DestroyNativeWindow(window_);
  window_ = NULL;
}

void EglDisplayImpl::Acquire() {
  Initialize();
  contexts_.Acquire();
  surfaces_.Acquire();
  color_buffers_.Acquire();
}

void EglDisplayImpl::Release() {
  contexts_.Release();
  surfaces_.Release();
  color_buffers_.Release();
}

EGLBoolean EglDisplayImpl::GetConfigs(EGLConfig* configs, EGLint config_size,
                                      EGLint* num_config) {
  const EGLint total_configs = GetNumConfigs();
  if (!configs) {
    *num_config = total_configs;
    return EGL_TRUE;
  }

  int count = 0;
  const EGLint max_configs = std::min(total_configs, config_size);
  for (ConfigSet::iterator i = configs_.begin(); i != configs_.end(); ++i) {
    if (count >= max_configs) {
      break;
    }
    configs[count] = i->GetKey();
    ++count;
  }
  LOG_ALWAYS_FATAL_IF(!num_config, "num_config must be non-NULL");
  *num_config = count;
  return EGL_TRUE;
}

EGLBoolean EglDisplayImpl::ChooseConfigs(const EGLint* attribs,
                                         EGLConfig* configs,
                                         EGLint configs_size,
                                         EGLint* num_config) {
  int count = 0;
  EglConfigImpl dummy(attribs);
  const EGLConfig egl_config = dummy.GetKey();
  if (egl_config != reinterpret_cast<EGLConfig>(EGL_DONT_CARE)) {
    const EglConfigImpl* cfg = GetConfig(egl_config);
    if (cfg && configs_size > 0) {
      configs[0] = cfg->GetKey();
      count = 1;
    } else {
      count = 0;
    }
  } else {
    for (ConfigSet::iterator i = configs_.begin(); i != configs_.end(); ++i) {
      if (i->Matches(dummy)) {
        if (configs) {
          if (count >= configs_size) {
            break;
          }
          configs[count] = i->GetKey();
        }
        ++count;
      }
    }
  }

  if (num_config) {
    *num_config = count;
  }

  return EGL_TRUE;
}

bool EglDisplayImpl::IsValidConfig(EGLConfig config) const {
  for (ConfigSet::const_iterator i = configs_.begin(); i != configs_.end();
       ++i) {
    if (config == i->GetKey()) {
      return true;
    }
  }
  return false;
}

EGLBoolean EglDisplayImpl::GetConfigAttribute(EGLConfig config, EGLint attrib,
                                              EGLint* value) {
  if (config && value) {
    *value = GetConfig(config)->GetValue(attrib);
    return EGL_TRUE;
  }
  return EGL_FALSE;
}

EGLBoolean EglDisplayImpl::GetConfigPixelFormat(EGLConfig config,
                                                EGLenum* format,
                                                EGLenum* type) {
  if (config && format && type) {
    GetConfig(config)->GetPixelFormat(format, type);
    return EGL_TRUE;
  }
  return EGL_FALSE;
}

EGLBoolean EglDisplayImpl::AreConfigsCompatible(EGLConfig lhs,
                                                EGLConfig rhs) const {
  if (lhs == NULL || rhs == NULL) {
    return EGL_FALSE;
  }
  const EglConfigImpl& a = *GetConfig(lhs);
  const EglConfigImpl& b = *GetConfig(rhs);
  return a.Compatible(b) ? EGL_TRUE : EGL_FALSE;
}

const EglConfigImpl* EglDisplayImpl::GetConfig(EGLConfig cfg) const {
  for (ConfigSet::const_iterator i = configs_.begin(); i != configs_.end();
       ++i) {
    if (i->GetKey() == cfg) {
      return &*i;
    }
  }
  return NULL;
}

bool EglDisplayImpl::Lock() {
  lock_.Lock();
  BindLocked();
  return true;
}

bool EglDisplayImpl::Unlock() {
  UnbindLocked();
  lock_.Unlock();
  return true;
}

void EglDisplayImpl::OnColorBufferAcquiredLocked() {
  ++color_buffers_locked_;
}

void EglDisplayImpl::OnColorBufferReleasedLocked() {
  --color_buffers_locked_;
  LOG_ALWAYS_FATAL_IF(color_buffers_locked_ < 0);
  if (!color_buffers_locked_) {
    cond_no_locked_buffers_.Signal();
  }
}

void EglDisplayImpl::OnGraphicsContextsLost() {
  lock_.Lock();

  LOG_ALWAYS_FATAL_IF(invalidated_);
  invalidated_ = true;

  while (color_buffers_locked_) {
    cond_no_locked_buffers_.Wait(lock_);
  }

  BindLocked();

  LOG_ALWAYS_FATAL_IF(!invalidated_);

  // color_buffers_ is not locked by display lock.
  ColorBufferRegistry::ObjectList color_buffers =
      color_buffers_.GetAllObjects();
  for (ColorBufferRegistry::ObjectList::const_iterator iter =
      color_buffers.begin(); iter != color_buffers.end(); ++iter) {
    (*iter)->DeleteTextureLocked();
  }

  ContextRegistry::ObjectList contexts = contexts_.GetAllObjects();
  for (ContextRegistry::ObjectList::const_iterator iter_context =
       contexts.begin(); iter_context != contexts.end(); ++iter_context) {
    (*iter_context)->GetGlesContext()->Invalidate();
  }

  UnbindLocked();
  lock_.Unlock();
}

void EglDisplayImpl::OnGraphicsContextsRestored() {
  Lock();

  LOG_ALWAYS_FATAL_IF(!invalidated_);
  LOG_ALWAYS_FATAL_IF(color_buffers_locked_);
  invalidated_ = false;

  ColorBufferRegistry::ObjectList color_buffers =
      color_buffers_.GetAllObjects();
  for (ColorBufferRegistry::ObjectList::const_iterator iter =
      color_buffers.begin(); iter != color_buffers.end(); ++iter) {
    (*iter)->CreateTextureLocked();
  }

  ContextRegistry::ObjectList contexts = contexts_.GetAllObjects();
  for (ContextRegistry::ObjectList::const_iterator iter_context =
       contexts.begin(); iter_context != contexts.end(); ++iter_context) {
    (*iter_context)->GetGlesContext()->Restore();
  }

  Unlock();
}

void EglDisplayImpl::BindLocked() {
  EglThreadInfo& info = EglThreadInfo::GetInstance();
  info.SaveCurrentContext();

  // Flush all operations of the current context before we switch to the
  // global context.
  ContextPtr curr_ctx = info.GetCurrentContext();
  if (curr_ctx != NULL) {
    curr_ctx->Flush();
  }

  ContextPtr next_ctx = contexts_.Get(global_context_);
  info.SetCurrentContext(next_ctx);
}

void EglDisplayImpl::UnbindLocked() {
  // Flush all remaining operations on the current context before we switch
  // back to the previous context.
  EglThreadInfo& info = EglThreadInfo::GetInstance();
  info.GetCurrentContext()->Flush();
  info.RestorePreviousContext();
}

void EglDisplayImpl::DrawFullscreenQuadLocked(GLuint texture, bool flip_v) {
  ContextPtr ctx = contexts_.Get(global_context_);
  ctx->GetGlesContext()->DrawFullscreenQuad(texture, flip_v);
}

void EglDisplayImpl::SwapBuffersLocked() {
  Native::SwapBuffers(window_);
}

EGLint EglDisplayImpl::SwapBuffers(EGLSurface egl_surface) {
  SurfacePtr sfc = surfaces_.Get(egl_surface);
  if (sfc == NULL) {
    return EGL_BAD_SURFACE;
  } else if (sfc->SwapBuffers()) {
    return EGL_SUCCESS;
  } else {
    return EGL_CONTEXT_LOST;
  }
}

EGLint EglDisplayImpl::MakeCurrent(EGLContext egl_ctx, EGLSurface egl_draw,
                                   EGLSurface egl_read) {
  if (egl_read != egl_draw) {
    LOG_ALWAYS_FATAL("Read and draw surfaces must be the same.");
    return EGL_BAD_MATCH;
  }

  ContextPtr ctx = contexts_.Get(egl_ctx);
  SurfacePtr sfc = surfaces_.Get(egl_draw);

  bool release = ctx == NULL && sfc == NULL;
  // If a context is being set, then a surface must be set.  Similarly, if a
  // context is being cleared, the surface must be cleared.  Any other
  // combination is an error.
  const bool invalid_surface = ctx != NULL ? sfc == NULL : sfc != NULL;
  if (!release && invalid_surface) {
    return EGL_BAD_MATCH;
  }

  EglThreadInfo& info = EglThreadInfo::GetInstance();
  ContextPtr prev_ctx = info.GetCurrentContext();
  SurfacePtr prev_sfc =
      prev_ctx != NULL ? prev_ctx->GetSurface() : SurfacePtr();

  if (release) {
    if (prev_ctx != NULL) {
      prev_ctx->Flush();
      info.SetCurrentContext(ContextPtr());
    }
  } else {
    if (ctx == NULL) {
      return EGL_BAD_CONTEXT;
    }
    if (ctx->config != sfc->config) {
      return EGL_BAD_MATCH;
    }
    if (ctx != NULL && prev_ctx != NULL) {
      if (ctx == prev_ctx) {
        if (sfc == prev_sfc) {
            // Reassigning the same context and surface.
            return EGL_SUCCESS;
        }
      } else {
        // Make sure to clear the previous context.
        release = true;
      }
    }

    if (prev_ctx != NULL) {
      prev_ctx->Flush();
    }

    if (!ctx->SetCurrent()) {
      return EGL_BAD_ACCESS;
    }

    info.SetCurrentContext(ctx);
    ctx->SetSurface(sfc);
  }

  if (prev_ctx != NULL && release) {
    prev_ctx->ClearCurrent();
    prev_ctx->ClearSurface();
  }
  return EGL_SUCCESS;
}
