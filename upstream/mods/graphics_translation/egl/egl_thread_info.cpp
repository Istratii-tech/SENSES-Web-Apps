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
#include "graphics_translation/egl/egl_thread_info.h"

#include <pthread.h>
#include <unistd.h>

#include "common/alog.h"
#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gles/pass_through.h"

static pthread_once_t tls_once_init = PTHREAD_ONCE_INIT;
static pthread_key_t tls_info;

static void thread_info_destructor(void* p) {
  EglThreadInfo* ptr = reinterpret_cast<EglThreadInfo*>(p);
  delete ptr;
}

static void tls_init() {
  pthread_key_create(&tls_info, thread_info_destructor);
}

EglThreadInfo::EglThreadInfo()
  : error_(EGL_SUCCESS),
    reported_no_context_error_(false),
    destroying_gles_context_(NULL) {
}

EglThreadInfo& EglThreadInfo::GetInstance() {
  pthread_once(&tls_once_init, tls_init);
  void* ptr = pthread_getspecific(tls_info);
  if (!ptr) {
    ptr = static_cast<void*>(new EglThreadInfo());
    pthread_setspecific(tls_info, ptr);
  }
  LOG_ALWAYS_FATAL_IF(!ptr);
  return *static_cast<EglThreadInfo*>(ptr);
}

void EglThreadInfo::SetError(EGLint error) {
  error_ = error;
}

EGLint EglThreadInfo::GetError() const {
  return error_;
}

ContextPtr EglThreadInfo::GetCurrentContext() {
  return curr_ctx_;
}

void EglThreadInfo::SetCurrentContext(ContextPtr ctx) {
  curr_ctx_ = ctx;
  // arc-wasm (topic open-surge-gl): a context lives in the WebGL of the thread
  // it is current on; made current on another thread than before, it moves
  // here now (egl/native_webgl.cpp, GlesContext::CarryObjectsToThisThread),
  // not in a GL call of the next operation, which may hold its share
  // group's lock.
  if (ctx != NULL && ctx->GetGlesContext() != NULL) {
    ArcWasmUseGlesContext(ctx->GetGlesContext());
  }
}

void EglThreadInfo::SaveCurrentContext() {
  prev_ctx_ = curr_ctx_;
}

void EglThreadInfo::RestorePreviousContext() {
  curr_ctx_ = prev_ctx_;
  prev_ctx_ = NULL;
}

bool EglThreadInfo::SetReportedNoContextError() {
  const bool prev = reported_no_context_error_;
  reported_no_context_error_ = true;
  return prev;
}

void EglThreadInfo::SetDestroyingGlesContext(GlesContext* context) {
  destroying_gles_context_ = context;
}

GlesContext* EglThreadInfo::GetDestroyingGlesContext() {
  return destroying_gles_context_;
}
