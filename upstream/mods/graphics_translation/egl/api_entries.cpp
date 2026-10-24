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
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <algorithm>

#include "common/alog.h"
#include "common/trace_event.h"
#include "graphics_translation/egl/egl_config_impl.h"
#include "graphics_translation/egl/egl_context_impl.h"
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/egl/egl_pbuffer_surface_impl.h"
#include "graphics_translation/egl/egl_surface_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/egl/egl_window_surface_impl.h"
#include "graphics_translation/egl/ext.h"
#include "graphics_translation/egl/native.h"
#include "graphics_translation/gralloc/graphics_buffer.h"
#include "system/window.h"
#include "utils/Timers.h"

// TODO(crbug.com/441903): Setup the logging in the same way as libgles?
#ifdef ENABLE_API_TRACING
#define EGL_TRACE() TRACE_EVENT0(ARC_TRACE_CATEGORY, __FUNCTION__) \
                    ALOGI("%s()", __FUNCTION__);
#else
#define EGL_TRACE()
#endif

#ifdef ENABLE_API_LOGGING
#define EGL_API_LOG(FMT, ...) ALOGI("%s(" FMT ")", __FUNCTION__, ## __VA_ARGS__)
#else
#define EGL_API_LOG(...)
#endif

#define EGL_API_ENTRY(...)    \
    EGL_TRACE();              \
    EGL_API_LOG(__VA_ARGS__);

#define EGL_API_UNIMPLEMENTED() \
    LOG_ALWAYS_FATAL("Unimplemented: %s", __FUNCTION__)

namespace {
const int kMajorVersion = 1;
const int kMinorVersion = 4;
const char* kClientApiString = "OpenGL_ES";
const char* kVendorString = "Chromium";
const char* kVersionString = "1.4";
const char* kExtensionString =
    "EGL_KHR_fence_sync "
    "EGL_KHR_image_base "
    "EGL_KHR_gl_texture_2d_image "
    "EGL_ANDROID_image_native_buffer "
    "EGL_NV_system_time ";
const EGLSyncKHR kFenceSyncHandle = (EGLSyncKHR)0xFE4CE;

// Utility function for getting the current thread's context.
ContextPtr GetContext() {
  return EglThreadInfo::GetInstance().GetCurrentContext();
}

// Utility function for readable string from error value.
const char* GetErrorString(EGLint err) {
  switch (err) {
    case EGL_SUCCESS:
      return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
      return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST:
      return "EGL_CONTEXT_LOST";
    default:
      return "EGL UNKNOWN ERROR";
  }
}

// Helper function for setting EGL errors.
void SetError(EGLint error) {
  if (error != EGL_SUCCESS) {
    ALOGE("EGL Error: %s %x", GetErrorString(error), error);
  }
  EglThreadInfo::GetInstance().SetError(error);
}

}  // namespace

// Returns a handle to an EGL display object.
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  EGL_API_ENTRY("%p", display_id);
  if (display_id != EGL_DEFAULT_DISPLAY) {
    ALOGE("ARC only supports EGL_DEFAULT_DISPLAY");
    return EGL_NO_DISPLAY;
  }
  return EglDisplayImpl::kDefaultDisplay;
}

// Initializes the specified EGL display object.
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
  EGL_API_ENTRY("%p, %p, %p", dpy, major, minor);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  if (major) {
    *major = kMajorVersion;
  }
  if (minor) {
    *minor = kMinorVersion;
  }
  display->Acquire();
  return EGL_TRUE;
}

// Release the EGL objects owned by the display.
// Note: The number of calls to eglTerminate must match the calls to
// eglInitialize in order to release the objects.
EGLBoolean eglTerminate(EGLDisplay dpy) {
  EGL_API_ENTRY("%p", dpy);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  display->Release();
  return EGL_TRUE;
}

// Get the error from the last called function in the current thread.
EGLint eglGetError() {
  const EGLint err = EglThreadInfo::GetInstance().GetError();
  EGL_API_ENTRY(") -> (returning %s [0x%x]", GetErrorString(err), err);
  EglThreadInfo::GetInstance().SetError(EGL_SUCCESS);
  return err;
}

// Return a string that describes the EGL system.
const char* eglQueryString(EGLDisplay dpy, EGLint name) {
  EGL_API_ENTRY("%p, %d", dpy, name);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return NULL;
  }
  switch (name) {
    case EGL_CLIENT_APIS:
      return kClientApiString;
    case EGL_VENDOR:
      return kVendorString;
    case EGL_VERSION:
      return kVersionString;
    case EGL_EXTENSIONS:
      return kExtensionString;
    default:
      SetError(EGL_BAD_PARAMETER);
      return NULL;
  }
}

// Same as eglQueryString.
const char* eglQueryStringImplementationANDROID(EGLDisplay dpy, EGLint name) {
  EGL_API_ENTRY("%p, %d", dpy, name);
  return eglQueryString(dpy, name);
}

// Sets the rendering API.
EGLBoolean eglBindAPI(EGLenum api) {
  EGL_API_ENTRY("0x%x", api);
  // ARC only supports GLES rendering.
  if (api != EGL_OPENGL_ES_API) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return EGL_TRUE;
}

// Query the current rendering API.
EGLenum eglQueryAPI() {
  EGL_API_ENTRY("");
  // ARC only supports GLES rendering.
  return EGL_OPENGL_ES_API;
}

// Returns a list of all configs supported by the display.
EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size,
                         EGLint* num_config) {
  EGL_API_ENTRY("%p, %p, %d, %p", dpy, configs, config_size, num_config);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  if (!num_config) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return display->GetConfigs(configs, config_size, num_config);
}

// Returns a list of configs that match the specified attributes.
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list,
                           EGLConfig* configs, EGLint config_size,
                           EGLint* num_config) {
  EGL_API_ENTRY("%p, %p, %p, %d, %p", dpy, attrib_list, configs, config_size,
                num_config);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  if (!num_config) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  if (display->ChooseConfigs(attrib_list, configs, config_size, num_config)) {
    return EGL_TRUE;
  } else {
    SetError(EGL_BAD_ATTRIBUTE);
    return EGL_FALSE;
  }
}

// Get the attribute value for the specified config.
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                              EGLint attribute, EGLint* value) {
  EGL_API_ENTRY("%p, %p, %d, %p", dpy, config, attribute, value);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  if (!display->IsValidConfig(config)) {
    SetError(EGL_BAD_CONFIG);
    return EGL_FALSE;
  }
  if (display->GetConfigAttribute(config, attribute, value)) {
    return EGL_TRUE;
  } else {
    SetError(EGL_BAD_ATTRIBUTE);
    return EGL_FALSE;
  }
}

// Create a window surface.
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint* attrib_list) {
  EGL_API_ENTRY("%p, %p, %p, %p", dpy, config, win, attrib_list);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  if (!display->IsValidConfig(config)) {
    SetError(EGL_BAD_CONFIG);
    return EGL_NO_SURFACE;
  }

  while (attrib_list && *attrib_list != EGL_NONE) {
    switch (attrib_list[0]) {
      case EGL_RENDER_BUFFER:
        if (attrib_list[1] != EGL_BACK_BUFFER) {
          ALOGW("eglCreateWindowSurface: Ignoring a setting of 0x%x for "
                "EGL_RENDER_BUFFER", attrib_list[1]);
        }
        break;

      default:
        LOG_ALWAYS_FATAL("Unknown attribute: 0x%x", attrib_list[0]);
        break;
    }
    attrib_list += 2;
  }

  EGLint error = EGL_SUCCESS;
  const EGLSurface surface = EglWindowSurfaceImpl::Create(
      dpy, config, static_cast<ANativeWindow*>(win), &error);
  if (surface == EGL_NO_SURFACE) {
    SetError(error);
  }
  return surface;
}

// Create a pbuffer surface.
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint* attrib_list) {
  EGL_API_ENTRY("%p, %p, %p", dpy, config, attrib_list);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  if (!display->IsValidConfig(config)) {
    SetError(EGL_BAD_CONFIG);
    return EGL_NO_SURFACE;
  }

  int32_t w = 0;
  int32_t h = 0;
  EGLint format = EGL_NO_TEXTURE;
  EGLint target = EGL_NO_TEXTURE;
  while (attrib_list && *attrib_list != EGL_NONE) {
    switch (attrib_list[0]) {
      case EGL_WIDTH:
        w = attrib_list[1];
        break;
      case EGL_HEIGHT:
        h = attrib_list[1];
        break;
      case EGL_TEXTURE_FORMAT:
        format = attrib_list[1];
        break;
      case EGL_TEXTURE_TARGET:
        target = attrib_list[1];
        break;
      default:
        LOG_ALWAYS_FATAL("Unknown attribute: %x", attrib_list[0]);
        break;
    }
    attrib_list += 2;
  }

  EGLint error = EGL_SUCCESS;
  const EGLSurface surface = EglPbufferSurfaceImpl::Create(
      dpy, config, w, h, format, target, &error);
  if (surface == EGL_NO_SURFACE) {
    SetError(error);
  }
  return surface;
}

// Create a pixmap surface.  Note: not supported.
EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativePixmapType pixmap,
                                  const EGLint* attrib_list) {
  EGL_API_UNIMPLEMENTED();
  return EGL_NO_SURFACE;
}

// Destroy the specified surface.
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  EGL_API_ENTRY("%p, %p", dpy, surface);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  display->GetSurfaces().Unregister(surface);
  return EGL_TRUE;
}

// Get the attribute value for the specified surface.
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                           EGLint* value) {
  EGL_API_ENTRY("%p, %p, 0x%x, %p", dpy, surface, attribute, value);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }

  switch (attribute) {
    case EGL_CONFIG_ID:
      return display->GetConfigAttribute(s->config, EGL_CONFIG_ID, value);
    case EGL_WIDTH:
      *value = s->GetWidth();
      return EGL_TRUE;
    case EGL_HEIGHT:
      *value = s->GetHeight();
      return EGL_TRUE;
    case EGL_TEXTURE_FORMAT:
      *value = s->GetTextureFormat();
      return EGL_TRUE;
    case EGL_TEXTURE_TARGET:
      *value = s->GetTextureTarget();
      return EGL_TRUE;
    case EGL_SWAP_BEHAVIOR:
      *value = EGL_BUFFER_DESTROYED;
      return EGL_TRUE;
    case EGL_LARGEST_PBUFFER:
      // Not modified for a window or pixmap surface.
      // Ignore it when creating a pbuffer surface.
      if (s->GetSurfaceType() & EGL_PBUFFER_BIT) {
        *value = EGL_FALSE;
      }
      return EGL_TRUE;
    default:
      LOG_ALWAYS_FATAL("Unsupported attribute: %x", attribute);
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_FALSE;
  }
}

// Set the attribute value for the specified surface.
EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
                            EGLint attribute, EGLint value) {
  EGL_API_ENTRY("%p, %p, 0x%x, %d", dpy, surface, attribute, value);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }

  switch (attribute) {
    case EGL_MIPMAP_LEVEL:
      if (value == 0) {
        return EGL_TRUE;
      }
      LOG_ALWAYS_FATAL("Unsupported attribute/value: %x %x", attribute, value);
      return EGL_FALSE;
    case EGL_MULTISAMPLE_RESOLVE:
      if (value == EGL_MULTISAMPLE_RESOLVE) {
        return EGL_TRUE;
      }
      LOG_ALWAYS_FATAL("Unsupported attribute/value: %x %x", attribute, value);
      return EGL_FALSE;
    case EGL_SWAP_BEHAVIOR:
      if (value == EGL_BUFFER_DESTROYED) {
        return EGL_TRUE;
      }
      LOG_ALWAYS_FATAL("Unsupported attribute/value: %x %x", attribute, value);
      return EGL_FALSE;
    default:
      ALOGE("Unsupported attribute: %x", attribute);
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_FALSE;
  }
}

// Retarget the current texture to the specified surface buffer.
EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
  EGL_API_ENTRY("%p, %p, %d", dpy, surface, buffer);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  if (buffer != EGL_BACK_BUFFER) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  if (s->GetTextureFormat() == EGL_NO_TEXTURE) {
    SetError(EGL_BAD_MATCH);
    return EGL_FALSE;
  }
  if (!(s->GetSurfaceType() & EGL_PBUFFER_BIT)) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  s->BindTexImage();
  return EGL_TRUE;
}

// Unbind the texture from the surface buffer.
EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface,
                              EGLint buffer) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

// Create a pbuffer surface from a client buffer.  Note: not supported.
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype,
                                            EGLClientBuffer buffer,
                                            EGLConfig config,
                                            const EGLint* attrib_list) {
  EGL_API_UNIMPLEMENTED();
  return EGL_NO_SURFACE;
}

// Create an EGL rendering context.
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint* attrib_list) {
  EGL_API_ENTRY("%p, %p, %p, %p", dpy, config, share_context, attrib_list);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_CONTEXT;
  }
  if (!display->IsValidConfig(config)) {
    SetError(EGL_BAD_CONFIG);
    return EGL_NO_CONTEXT;
  }

  EGLint version = 1;  // Create GLES1 context by default.
  while (attrib_list && attrib_list[0] != EGL_NONE) {
    if (attrib_list[0] == EGL_CONTEXT_CLIENT_VERSION) {
      version = attrib_list[1];
    }
    attrib_list += 2;
  }

  EGLint error = EGL_SUCCESS;
  const EGLContext context =
      EglContextImpl::Create(dpy, config, share_context, version, &error);
  if (context == EGL_NO_CONTEXT) {
    SetError(error);
  }
  return context;
}

// Destroy the specified rendering context.
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  EGL_API_ENTRY("%p, %p", dpy, ctx);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }

  ContextPtr context = display->GetContexts().Get(ctx);
  if (context == NULL) {
    return EGL_BAD_CONTEXT;
  }

  if (GetContext() == context) {
    display->MakeCurrent(EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE);
  }

  context->Release();

  display->GetContexts().Unregister(ctx);
  return EGL_TRUE;
}

// Attach an EGL rendering context to the specified surfaces.
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx) {
  EGL_API_ENTRY("%p, %p, %p, %p", dpy, draw, read, ctx);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL) {
    ALOGW("eglMakeCurrent called with invalid display. "
          "Using default display instead. (draw=%p read=%p ctx=%p)",
          draw, read, ctx);
    display = EglDisplayImpl::GetDefaultDisplay();
  }

  if (display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }

  const EGLint error = display->MakeCurrent(ctx, draw, read);
  if (error != EGL_SUCCESS) {
    SetError(error);
    return EGL_FALSE;
  } else {
    return EGL_TRUE;
  }
}

// Release per-thread state.
EGLBoolean eglReleaseThread() {
  EGL_API_ENTRY("");
  SetError(EGL_SUCCESS);
  return eglMakeCurrent(EglDisplayImpl::kDefaultDisplay, EGL_NO_CONTEXT,
                        EGL_NO_SURFACE, EGL_NO_SURFACE);
}

// Get the current rendering context.
EGLContext eglGetCurrentContext() {
  EGL_API_ENTRY("");
  ContextPtr context = GetContext();
  if (context == NULL) {
    return EGL_NO_CONTEXT;
  }
  return context->GetKey();
}

// Get the display associated with the current rendering context.
EGLDisplay eglGetCurrentDisplay() {
  EGL_API_ENTRY("");
  ContextPtr context = GetContext();
  if (context == NULL) {
    return EGL_NO_DISPLAY;
  }
  return context->display;
}

// Get the surface associated with the current rendering context.
EGLSurface eglGetCurrentSurface(EGLint readdraw) {
  EGL_API_ENTRY("0x%x", readdraw);
  if (readdraw != EGL_READ && readdraw != EGL_DRAW) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_SURFACE;
  }
  ContextPtr context = GetContext();
  if (context == NULL) {
    return EGL_NO_SURFACE;
  }
  SurfacePtr surface = context->GetSurface();
  if (surface == NULL) {
    return EGL_NO_SURFACE;
  }
  return surface->GetKey();
}

// Get the attribute value for the specified rendering context.
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute,
                           EGLint* value) {
  EGL_API_ENTRY("%p, %p, 0x%x, %p", dpy, ctx, attribute, value);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  ContextPtr context = display->GetContexts().Get(ctx);
  if (context == NULL) {
    SetError(EGL_BAD_CONTEXT);
    return EGL_FALSE;
  }

  switch (attribute) {
    case EGL_CONFIG_ID:
      return display->GetConfigAttribute(context->config, attribute, value);
    case EGL_CONTEXT_CLIENT_TYPE:
      *value = EGL_OPENGL_ES_API;
      return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_VERSION:
      *value = context->GetVersion();
      return EGL_TRUE;
    case EGL_RENDER_BUFFER:
      if (context->GetSurface() == NULL)
        *value = EGL_NONE;
      else
        *value = EGL_BACK_BUFFER;  // single buffer not supported
      return EGL_TRUE;
    case EGL_ARC_UNDERLYING_CONTEXT: {
      NativeContext* native = context->GetNativeContext();
      *value = reinterpret_cast<EGLint>(Native::GetUnderlyingContext(native));
      return EGL_TRUE;
    }
    default:
      ALOGE("Unsupported attribute: %x", attribute);
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_FALSE;
  }
}

// Set the swap interval for the current draw surface.
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  EGL_API_ENTRY("%p, %d", dpy, interval);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  ContextPtr context = GetContext();
  if (context == NULL) {
    SetError(EGL_BAD_CONTEXT);
    return EGL_FALSE;
  }
  if (context->GetSurface() == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  interval = ClampValue<EGLint>(interval, EglConfigImpl::kMinSwapInterval,
                                EglConfigImpl::kMaxSwapInterval);
  context->GetSurface()->SetSwapInterval(interval);
  return EGL_TRUE;
}

// Post EGL surface color buffer to a native window.
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  EGL_API_ENTRY("%p, %p", dpy, surface);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }

  const EGLint error = display->SwapBuffers(surface);
  if (error != EGL_SUCCESS) {
    SetError(error);
    return EGL_FALSE;
  } else {
    return EGL_TRUE;
  }
}

// Create an EGL image from the specified ANativeWindowBuffer.
EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                              EGLClientBuffer buffer,
                              const EGLint* attrib_list) {
  EGL_API_ENTRY("%p, %p, 0x%x, %p, %p", dpy, ctx, target, buffer, attrib_list);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_IMAGE_KHR;
  }
  if (buffer == EGL_NO_IMAGE_KHR) {
    return buffer;
  }
  // Android only requires support for EGL_ANDROID_image_native_buffer.
  if (target != EGL_NATIVE_BUFFER_ANDROID) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_IMAGE_KHR;
  }

  ANativeWindowBuffer* native_buffer =
      reinterpret_cast<ANativeWindowBuffer*>(buffer);
  if (!IsValidNativeWindowBuffer(native_buffer)) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_IMAGE_KHR;
  }

  const GraphicsBuffer* cb =
      reinterpret_cast<const GraphicsBuffer*>(native_buffer->handle);
  switch (cb->GetFormat()) {
    case HAL_PIXEL_FORMAT_BGRA_8888:
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_RGB_888:
    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_YV12:
      break;
    default:
      SetError(EGL_BAD_PARAMETER);
      return EGL_NO_IMAGE_KHR;
  }
  // Increment the reference count to ensure the native buffer is not destroyed
  // while it is being used as an EGL image.
  native_buffer->common.incRef(&native_buffer->common);
  return (EGLImageKHR)native_buffer;
}

// Destroy the specified EGL image.
EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR img) {
  EGL_API_ENTRY("%p, %p", dpy, img);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  if (img == EGL_NO_IMAGE_KHR) {
    return EGL_TRUE;
  }

  ANativeWindowBuffer* native_buffer =
      reinterpret_cast<ANativeWindowBuffer*>(img);
  if (!IsValidNativeWindowBuffer(native_buffer)) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  native_buffer->common.decRef(&native_buffer->common);
  return EGL_TRUE;
}

EGLBoolean eglWaitClient() {
  EGL_API_UNIMPLEMENTED();
  return EGL_TRUE;
}

EGLBoolean eglWaitGL() {
  EGL_API_UNIMPLEMENTED();
  return EGL_TRUE;
}

EGLBoolean eglWaitNative(EGLint engine) {
  EGL_API_UNIMPLEMENTED();
  return EGL_TRUE;
}

EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface,
                          EGLNativePixmapType target) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

EGLBoolean eglLockSurfaceKHR(EGLDisplay dpy, EGLSurface surface,
                             const EGLint* attrib_list) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

EGLBoolean eglUnlockSurfaceKHR(EGLDisplay dpy, EGLSurface surface) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

// Create a reusable EGL sync object.
EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type,
                            const EGLint* attrib_list) {
  EGL_API_ENTRY("%p, 0x%x, %p", dpy, type, attrib_list);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_SYNC_KHR;
  }
  if (type != EGL_SYNC_FENCE_KHR ||
      (attrib_list != NULL && attrib_list[0] != EGL_NONE)) {
    SetError(EGL_BAD_ATTRIBUTE);
    return EGL_NO_SYNC_KHR;
  }

  ContextPtr context = GetContext();
  if (context == NULL) {
    SetError(EGL_BAD_MATCH);
    return EGL_NO_SYNC_KHR;
  }

  glFinish();
  return kFenceSyncHandle;
}

// Destroy the specified sync object.
EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync) {
  EGL_API_ENTRY("%p, %p", dpy, sync);
  if (sync != kFenceSyncHandle) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return EGL_TRUE;
}

EGLint eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

// Wait for the sync.
EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags,
                            EGLTimeKHR timeout) {
  EGL_API_ENTRY("%p, %p, %d, %llu", dpy, sync, flags, timeout);
  if (sync != kFenceSyncHandle) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  // We call glFinish when the sync object is acquired so there should be
  // nothing to wait for anymore.
  SetError(EGL_SUCCESS);
  return EGL_CONDITION_SATISFIED_KHR;
}

// Get the attribute value of the specified sync object.
EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync,
                               EGLint attribute, EGLint* value) {
  EGL_API_ENTRY("%p, %p, 0x%x, %p", dpy, sync, attribute, value);
  if (sync != kFenceSyncHandle) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  switch (attribute) {
    case EGL_SYNC_TYPE_KHR:
      *value = EGL_SYNC_FENCE_KHR;
      return EGL_TRUE;
    case EGL_SYNC_STATUS_KHR:
      *value = EGL_SIGNALED_KHR;
      return EGL_TRUE;
    case EGL_SYNC_CONDITION_KHR:
      *value = EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
      return EGL_TRUE;
    default:
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_FALSE;
  }
}

// Set the timestamp for the specified surface.
void eglBeginFrame(EGLDisplay dpy, EGLSurface surface) {
  EGL_API_ENTRY("%p, %p", dpy, surface);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return;
  }
  const int64_t timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
  s->SetTimestamp(timestamp);
}

EGLBoolean eglPresentationTimeANDROID(EGLDisplay dpy, EGLSurface surface,
                                      EGLnsecsANDROID time) {
  EGL_API_ENTRY("%p, %p, %lld", dpy, surface, time);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL || display->IsInitialized() == false) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }
  SurfacePtr s = display->GetSurfaces().Get(surface);
  if (s == NULL) {
    SetError(EGL_BAD_SURFACE);
    return EGL_FALSE;
  }
  s->SetTimestamp(time);
  return EGL_TRUE;
}

EGLuint64NV eglGetSystemTimeNV() {
  // Returns time in nano seconds.
  return systemTime(SYSTEM_TIME_MONOTONIC);
}

EGLuint64NV eglGetSystemTimeFrequencyNV() {
  // Number of "ticks" per second.
  return seconds_to_nanoseconds(1);
}

EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync) {
  EGL_API_UNIMPLEMENTED();
  return 0;
}

EGLBoolean eglContextsLostARC(EGLDisplay dpy) {
  EGL_API_ENTRY("%p", dpy);
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  if (display == NULL) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_FALSE;
  }

  display->OnGraphicsContextsLost();

  return EGL_TRUE;
}

EGLBoolean eglContextsRestoredARC(EGLDisplay dpy) {
    EGL_API_ENTRY("%p", dpy);
    EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
    if (display == NULL) {
      SetError(EGL_BAD_DISPLAY);
      return EGL_FALSE;
    }

    display->OnGraphicsContextsRestored();

    return EGL_TRUE;
}

// Return the EGL function specified by the name.
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
#define LOOKUP_EGL_FUNCTION(_fn)                                           \
  if (!strcmp(name, #_fn))                                                 \
    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(_fn)

  LOOKUP_EGL_FUNCTION(eglGetError);
  LOOKUP_EGL_FUNCTION(eglGetDisplay);
  LOOKUP_EGL_FUNCTION(eglInitialize);
  LOOKUP_EGL_FUNCTION(eglTerminate);
  LOOKUP_EGL_FUNCTION(eglQueryString);
  LOOKUP_EGL_FUNCTION(eglQueryStringImplementationANDROID);
  LOOKUP_EGL_FUNCTION(eglGetConfigs);
  LOOKUP_EGL_FUNCTION(eglChooseConfig);
  LOOKUP_EGL_FUNCTION(eglGetConfigAttrib);
  LOOKUP_EGL_FUNCTION(eglCreateWindowSurface);
  LOOKUP_EGL_FUNCTION(eglCreatePbufferSurface);
  LOOKUP_EGL_FUNCTION(eglCreatePixmapSurface);
  LOOKUP_EGL_FUNCTION(eglDestroySurface);
  LOOKUP_EGL_FUNCTION(eglQuerySurface);
  LOOKUP_EGL_FUNCTION(eglBindAPI);
  LOOKUP_EGL_FUNCTION(eglQueryAPI);
  LOOKUP_EGL_FUNCTION(eglWaitClient);
  LOOKUP_EGL_FUNCTION(eglReleaseThread);
  LOOKUP_EGL_FUNCTION(eglCreatePbufferFromClientBuffer);
  LOOKUP_EGL_FUNCTION(eglSurfaceAttrib);
  LOOKUP_EGL_FUNCTION(eglBindTexImage);
  LOOKUP_EGL_FUNCTION(eglReleaseTexImage);
  LOOKUP_EGL_FUNCTION(eglSwapInterval);
  LOOKUP_EGL_FUNCTION(eglCreateContext);
  LOOKUP_EGL_FUNCTION(eglDestroyContext);
  LOOKUP_EGL_FUNCTION(eglMakeCurrent);
  LOOKUP_EGL_FUNCTION(eglGetCurrentContext);
  LOOKUP_EGL_FUNCTION(eglGetCurrentSurface);
  LOOKUP_EGL_FUNCTION(eglGetCurrentDisplay);
  LOOKUP_EGL_FUNCTION(eglQueryContext);
  LOOKUP_EGL_FUNCTION(eglWaitGL);
  LOOKUP_EGL_FUNCTION(eglWaitNative);
  LOOKUP_EGL_FUNCTION(eglSwapBuffers);
  LOOKUP_EGL_FUNCTION(eglCopyBuffers);
  LOOKUP_EGL_FUNCTION(eglGetProcAddress);
  LOOKUP_EGL_FUNCTION(eglLockSurfaceKHR);
  LOOKUP_EGL_FUNCTION(eglUnlockSurfaceKHR);
  LOOKUP_EGL_FUNCTION(eglCreateImageKHR);
  LOOKUP_EGL_FUNCTION(eglDestroyImageKHR);
  LOOKUP_EGL_FUNCTION(eglCreateSyncKHR);
  LOOKUP_EGL_FUNCTION(eglDestroySyncKHR);
  LOOKUP_EGL_FUNCTION(eglClientWaitSyncKHR);
  LOOKUP_EGL_FUNCTION(eglGetSyncAttribKHR);
  LOOKUP_EGL_FUNCTION(eglPresentationTimeANDROID);
  LOOKUP_EGL_FUNCTION(eglContextsLostARC);
  LOOKUP_EGL_FUNCTION(eglContextsRestoredARC);
#undef LOOKUP_EGL_FUNCTION
  return NULL;
}
