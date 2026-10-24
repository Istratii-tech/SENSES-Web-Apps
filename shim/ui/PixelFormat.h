// Subset of AOSP frameworks/native ui/PixelFormat.h (values mirror the HAL
// formats in system/graphics.h). Part of the arc-wasm shim layer.
#ifndef UI_PIXELFORMAT_H
#define UI_PIXELFORMAT_H

#include <stdint.h>

#include <system/graphics.h>

namespace android {

enum {
  PIXEL_FORMAT_UNKNOWN = 0,
  PIXEL_FORMAT_NONE = 0,
  PIXEL_FORMAT_CUSTOM = -4,
  PIXEL_FORMAT_TRANSLUCENT = -3,
  PIXEL_FORMAT_TRANSPARENT = -2,
  PIXEL_FORMAT_OPAQUE = -1,
  PIXEL_FORMAT_RGBA_8888 = HAL_PIXEL_FORMAT_RGBA_8888,
  PIXEL_FORMAT_RGBX_8888 = HAL_PIXEL_FORMAT_RGBX_8888,
  PIXEL_FORMAT_RGB_888 = HAL_PIXEL_FORMAT_RGB_888,
  PIXEL_FORMAT_RGB_565 = HAL_PIXEL_FORMAT_RGB_565,
  PIXEL_FORMAT_BGRA_8888 = HAL_PIXEL_FORMAT_BGRA_8888,
  PIXEL_FORMAT_RGBA_5551 = 6,
  PIXEL_FORMAT_RGBA_4444 = 7,
};

typedef int32_t PixelFormat;

}  // namespace android

#endif  // UI_PIXELFORMAT_H
