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
#ifndef GRAPHICS_TRANSLATION_EGL_EXT_H_
#define GRAPHICS_TRANSLATION_EGL_EXT_H_

// ARC Extension macros that are used to expose ARC-specific functionality.

// When used with eglQueryContext(), can be used to get a pointer to the
// arc::ContextGPU backing the EGL context.
#define EGL_ARC_UNDERLYING_CONTEXT   0x7fff0001

// Used to handle situation when external Chrome graphics contexts are lost.
EGLAPI EGLBoolean EGLAPIENTRY eglContextsLostARC(EGLDisplay dpy);
EGLAPI EGLBoolean EGLAPIENTRY eglContextsRestoredARC(EGLDisplay dpy);

#endif  // GRAPHICS_TRANSLATION_EGL_EXT_H_
