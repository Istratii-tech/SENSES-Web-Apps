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
#include <gralloc/gralloc.h>
#include <hardware/hwcomposer.h>
#include <utils/Errors.h>

#include "common/options.h"
#include "common/plugin_handle.h"
#include "gralloc/graphics_buffer.h"

using arc::CompositorInterface;
typedef arc::CompositorInterface::Display Display;
typedef arc::CompositorInterface::Layer Layer;
typedef arc::CompositorInterface::FloatRect FloatRect;
typedef arc::CompositorInterface::Rect Rect;

struct hwc_context_t {
  hwc_context_t();
  ~hwc_context_t();

  hwc_composer_device_1_t device;
  CompositorInterface::Display display;
  CompositorInterface* compositor;
  const hwc_procs_t* procs;

  // These 3 variables could be reduced to first_overlay only, however it makes
  // the conditions in the code more complicated. In order to keep things as
  // simple as possible, there are 3 major ways to display a frame.
  // 1. Show only the framebuffer.
  // 2. Show the framebuffer with some overlays above it.
  // 3. Show all overlays and hide the framebuffer.
  //
  // Since the framebuffer has no alpha channel and is opaque, it can only ever
  // be the rearmost layer that we end up putting on screen, otherwise it will
  // cover up all layers behind it, since its display frame is the whole window.
  //
  // Without framebuffer_visible, the condition of whether to display the
  // frambuffer becomes more complex and possibly if (numHwLayers == 0 ||
  // hwLayers[0]->compositionType != HWC_OVERLAY) but that might not be correct.
  //
  // The range [first_overlay, first_overlay+num_overlay) is a natural way to
  // structure the loop and prevents requiring state and iterating through all
  // the non-OVERLAY layers in hwc_set.
  bool framebuffer_visible;
  size_t first_overlay;
  size_t num_overlays;

};

hwc_context_t::hwc_context_t() :
#ifdef __clang__
    // Initialize const member variables in the struct. Otherwise, this file
    // does not compile with clang. Note that this is C++11 style
    // initialization.
    // TODO(crbug.com/365178): Remove the ifdef once C++11 is enabled for GCC.
    device({}),
#endif
    compositor(NULL),
    procs(0),
    framebuffer_visible(false),
    first_overlay(0),
    num_overlays(0) {
}

hwc_context_t::~hwc_context_t() {
}

static const GraphicsBuffer* GetGraphicsBuffer(buffer_handle_t handle) {
  return static_cast<const GraphicsBuffer*>(handle);
}

static FloatRect MakeFloatRect(const hwc_frect_t& in) {
  const FloatRect r = { in.left, in.top, in.right, in.bottom };
  return r;
}

static Rect MakeRect(const hwc_rect_t& in) {
  const Rect r = { in.left, in.top, in.right, in.bottom };
  return r;
}

static void UpdateLayer(Layer* layer, hwc_layer_1_t* hw_layer) {
  switch(layer->type) {
    case Layer::TYPE_TEXTURE: {
      const GraphicsBuffer* buffer = GetGraphicsBuffer(hw_layer->handle);
      // The buffer is upside down, if it is rendered by software.
      const int need_flip_flags = GRALLOC_USAGE_SW_WRITE_MASK |
                                  GRALLOC_USAGE_HW_CAMERA_WRITE |
                                  GRALLOC_USAGE_ARC_SYSTEM_TEXTURE;
      layer->need_flip = buffer->GetUsage() & need_flip_flags;
      layer->texture.target = buffer->GetHostTarget();
      layer->texture.name = buffer->GetHostTexture();
      layer->context = buffer->GetHostContext();
      layer->alpha = hw_layer->planeAlpha;
      layer->is_opaque = hw_layer->blending == HWC_BLENDING_NONE ||
          hw_layer->planeAlpha == 255;
      break;
    }
    case Layer::TYPE_SOLID_COLOR: {
      const hwc_color_t& color = hw_layer->backgroundColor;
      layer->color =
          Layer::PackColor(color.r, color.g, color.b, color.a);
      break;
    }
  }
}

static void UpdateDisplay(const hwc_context_t* context,
                          Display* display,
                          hwc_display_contents_1_t* hw_display) {
  std::vector<Layer>::iterator layers_it = display->layers.begin();
  if (context->framebuffer_visible) {
    hwc_layer_1_t* layer = &hw_display->hwLayers[hw_display->numHwLayers - 1];
    UpdateLayer(&(*layers_it), layer);
    ++layers_it;
  }
  for (size_t i = 0; i != context->num_overlays; ++i) {
    hwc_layer_1_t* layer = &hw_display->hwLayers[context->first_overlay + i];
    UpdateLayer(&(*layers_it), layer);
    ++layers_it;
  }
  if (layers_it != display->layers.end()) {
    ALOGE("Unexpected number of layers updated");
  }
}

static Layer MakeLayer(hwc_layer_1_t* hw_layer) {
  Layer layer;
  switch(hw_layer->compositionType) {
    case HWC_FRAMEBUFFER_TARGET:
    case HWC_OVERLAY: {
      const GraphicsBuffer* buffer = GetGraphicsBuffer(hw_layer->handle);
      layer.size.width = buffer->GetWidth();
      layer.size.height = buffer->GetHeight();
      layer.type = Layer::TYPE_TEXTURE;
      layer.source = MakeFloatRect(hw_layer->sourceCropf);
      layer.dest = MakeRect(hw_layer->displayFrame);
      layer.transform = hw_layer->transform;
      layer.releaseFenceFd = &hw_layer->releaseFenceFd;
      break;
    }
    case HWC_BACKGROUND: {
      layer.type = Layer::TYPE_SOLID_COLOR;
      layer.releaseFenceFd = NULL;
      break;
    }
    default: {
      ALOGE("Unexpected layer type: %d", hw_layer->compositionType);
      // Make sure we have a deterministic value, a solid black layer.
      layer.type = Layer::TYPE_SOLID_COLOR;
      layer.color = Layer::PackColor(0, 0, 0, 255);
      return layer;
    }
  }
  UpdateLayer(&layer, hw_layer);
  return layer;
}

static Display MakeDisplay(hwc_context_t* context,
                           hwc_display_contents_1_t* hw_display) {
  Display display;
  display.layers.reserve(context->num_overlays + 1);
  if (context->framebuffer_visible) {
    hwc_layer_1_t* layer = &hw_display->hwLayers[hw_display->numHwLayers - 1];
    display.layers.push_back(MakeLayer(layer));
  }

  for (size_t i = 0; i != context->num_overlays; ++i) {
    hwc_layer_1_t* layer = &hw_display->hwLayers[context->first_overlay + i];
    display.layers.push_back(MakeLayer(layer));
  }
  return display;
}

static int hwc_prepare(hwc_composer_device_1_t* dev, size_t numDisplays,
                       hwc_display_contents_1_t** displays) {
  ALOGD("HWC_PREPARE");
  hwc_context_t* context = reinterpret_cast<hwc_context_t*>(dev);
  if (displays == NULL || displays[0] == NULL) {
    return -EINVAL;
  }

  // ARC Only supports the primary display.
  if (displays[0]->flags & HWC_GEOMETRY_CHANGED) {
    const size_t& numHwLayers = displays[0]->numHwLayers;
    size_t i = 1;
    bool visible = (numHwLayers == 1);
    // Iterate backwards and skip the first (end) layer, which is the
    // framebuffer target layer. According to the SurfaceFlinger folks, the
    // actual location of this layer is up to the HWC implementation to
    // decide, but is in the well know last slot of the list. This does not
    // imply that the framebuffer target layer must be topmost.
    for (; i < numHwLayers; i++) {
      hwc_layer_1_t* layer = &displays[0]->hwLayers[numHwLayers - 1 - i];
      if (layer->flags & HWC_SKIP_LAYER) {
        // All layers below and including this one will be drawn into the
        // framebuffer. Stop marking further layers as HWC_OVERLAY.
        visible = true;
        break;
      }
      switch (layer->compositionType) {
        case HWC_OVERLAY:
        case HWC_FRAMEBUFFER:
          layer->compositionType = HWC_OVERLAY;
          break;
        case HWC_BACKGROUND:
          break;
        default:
          ALOGE("hwcomposor: Invalid compositionType %d",
                  layer->compositionType);
          break;
      }
    }
    context->first_overlay = numHwLayers - i;
    context->num_overlays = i - 1;
    context->framebuffer_visible = visible;
  }
  return 0;
}

static int hwc_set(hwc_composer_device_1_t* dev, size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
  ALOGD("HWC_SET");
  hwc_context_t* context = reinterpret_cast<hwc_context_t*>(dev);
  if (displays == NULL || displays[0] == NULL) {
    return -EFAULT;
  }

  if (displays[0]->flags & HWC_GEOMETRY_CHANGED) {
    context->display = MakeDisplay(context, displays[0]);
  } else {
    UpdateDisplay(context, &context->display, displays[0]);
  }

  int fd = context->compositor->Set(&context->display);
  displays[0]->retireFenceFd = fd;
  return 0;
}

static int hwc_event_control(hwc_composer_device_1* dev, int disp,
                             int event, int enabled) {
  return -EFAULT;
}

static int hwc_get_display_configs(hwc_composer_device_1* dev, int disp,
                                   uint32_t* configs, size_t* numConfigs) {
  if (disp != 0) {
    return -EINVAL;
  }

  if (*numConfigs > 0) {
    // Config[0] will be passed in to getDisplayAttributes as the disp
    // parameter. The ARC display supports only 1 configuration.
    configs[0] = 0;
    *numConfigs = 1;
  }
  return 0;
}

static int hwc_get_display_attributes(hwc_composer_device_1* dev,
                                      int disp, uint32_t config,
                                      const uint32_t* attributes,
                                      int32_t* values) {
  if (disp != 0 || config != 0) {
    return -EINVAL;
  }

  arc::PluginHandle handle;
  arc::RendererInterface::RenderParams params;
  handle.GetRenderer()->GetRenderParams(&params);

  hwc_context_t* context = reinterpret_cast<hwc_context_t*>(dev);
  while (*attributes != HWC_DISPLAY_NO_ATTRIBUTE) {
    switch (*attributes) {
      case HWC_DISPLAY_VSYNC_PERIOD:
        *values = params.vsync_period;
        break;
      case HWC_DISPLAY_WIDTH:
        *values = params.width;
        break;
      case HWC_DISPLAY_HEIGHT:
        *values = params.height;
        break;
      case HWC_DISPLAY_DPI_X:
        *values = 1000 * params.display_density;
        break;
      case HWC_DISPLAY_DPI_Y:
        *values = 1000 * params.display_density;
        break;
      default:
        ALOGE("Unknown attribute value 0x%02x", *attributes);
    }
    ++attributes;
    ++values;
  }
  return 0;
}

static void hwc_register_procs(hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
}

static int hwc_blank(hwc_composer_device_1* dev, int disp, int blank) {
  return 0;
}

static int hwc_query(hwc_composer_device_1* dev, int what, int* value) {
  return 0;
}

static int hwc_device_close(hw_device_t* dev) {
  hwc_context_t* context = reinterpret_cast<hwc_context_t*>(dev);
  delete context;
  return 0;
}

static int hwc_device_open(const hw_module_t* module, const char* name,
                           hw_device_t** device) {
  arc::PluginHandle handle;
  if (!handle.GetRenderer() || !handle.GetRenderer()->GetCompositor()) {
    return -ENODEV;
  }

  if (strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
    return -EINVAL;
  }

  hwc_context_t* dev = new hwc_context_t();
  dev->device.common.tag = HARDWARE_DEVICE_TAG;
  dev->device.common.version = HWC_DEVICE_API_VERSION_1_3;
  dev->device.common.module = const_cast<hw_module_t*>(module);
  dev->device.common.close = hwc_device_close;
  dev->device.prepare = hwc_prepare;
  dev->device.set = hwc_set;
  dev->device.eventControl = hwc_event_control;
  dev->device.blank = hwc_blank;
  dev->device.query = hwc_query;
  dev->device.getDisplayConfigs = hwc_get_display_configs;
  dev->device.getDisplayAttributes = hwc_get_display_attributes;
  dev->device.registerProcs = hwc_register_procs;
  dev->compositor = handle.GetRenderer()->GetCompositor();
  *device = &dev->device.common;
  return 0;
}

static hw_module_methods_t hwc_module_methods = {
  // TODO(crbug.com/365178): Switch to ".member = value" style.
  open : hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
  // TODO(crbug.com/365178): Switch to ".member = value" style.
  common : {
    tag : HARDWARE_MODULE_TAG,
    version_major : 1,
    version_minor : 0,
    id : HWC_HARDWARE_MODULE_ID,
    name : "Hardware Composer Module",
    author: "chromium.org",
    methods : &hwc_module_methods,
  }
};
