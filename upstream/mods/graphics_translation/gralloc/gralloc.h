/*
 * Copyright (C) 2008 The Android Open Source Project
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
#ifndef GRAPHICS_TRANSLATION_GRALLOC_GRALLOC_H_
#define GRAPHICS_TRANSLATION_GRALLOC_GRALLOC_H_

#include "hardware/gralloc.h"

#define GRALLOC_USAGE_ARC_SYSTEM_TEXTURE GRALLOC_USAGE_PRIVATE_0

extern "C" {
int gralloc_register_buffer(gralloc_module_t const* module,
                            buffer_handle_t handle);

int gralloc_unregister_buffer(gralloc_module_t const* module,
                              buffer_handle_t handle);

int gralloc_lock(gralloc_module_t const* module, buffer_handle_t handle,
                 int usage, int l, int t, int w, int h, void** vaddr);

int gralloc_unlock(gralloc_module_t const* module, buffer_handle_t handle);

int gralloc_device_open(const hw_module_t* module, const char* name,
                        hw_device_t** device);
}  // extern "C"

#endif  // GRAPHICS_TRANSLATION_GRALLOC_GRALLOC_H_
