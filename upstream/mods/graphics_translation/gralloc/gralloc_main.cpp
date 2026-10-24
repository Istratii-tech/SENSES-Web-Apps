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
#include "graphics_translation/gralloc/gralloc.h"
#include "hardware/gralloc.h"

struct private_module_t {
  gralloc_module_t base;
};

static struct hw_module_methods_t gralloc_module_methods = {
  // TODO(crbug.com/365178): Switch to ".member = value" style.
  open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
  // TODO(crbug.com/365178): Switch to ".member = value" style.
  base: {
    common: {
      tag: HARDWARE_MODULE_TAG,
      version_major: 1,
      version_minor: 0,
      id: GRALLOC_HARDWARE_MODULE_ID,
      name: "Graphics Memory Allocator Module",
      author: "chromium.org",
      methods: &gralloc_module_methods,
      dso: NULL,
    },
    registerBuffer: gralloc_register_buffer,
    unregisterBuffer: gralloc_unregister_buffer,
    lock: gralloc_lock,
    unlock: gralloc_unlock,
    perform: NULL,
  }
};
