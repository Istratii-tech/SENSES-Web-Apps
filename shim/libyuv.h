// The one libyuv routine ARC's gralloc uses (YV12 software buffers), as a
// straightforward BT.601 conversion. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_LIBYUV_H_
#define ARC_WASM_SHIM_LIBYUV_H_

#include <stddef.h>
#include <stdint.h>

namespace libyuv {

inline uint8_t ClampByte(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Writes R, G, B, A bytes in memory order (libyuv's "ABGR" naming).
inline int I420ToABGR(const uint8_t* src_y, int src_stride_y,
                      const uint8_t* src_u, int src_stride_u,
                      const uint8_t* src_v, int src_stride_v,
                      uint8_t* dst_abgr, int dst_stride_abgr,
                      int width, int height) {
  for (int y = 0; y < height; ++y) {
    uint8_t* dst = dst_abgr + y * dst_stride_abgr;
    for (int x = 0; x < width; ++x) {
      const int c = src_y[y * src_stride_y + x] - 16;
      const int d = src_u[(y / 2) * src_stride_u + x / 2] - 128;
      const int e = src_v[(y / 2) * src_stride_v + x / 2] - 128;
      dst[4 * x + 0] = ClampByte((298 * c + 409 * e + 128) >> 8);
      dst[4 * x + 1] = ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
      dst[4 * x + 2] = ClampByte((298 * c + 516 * d + 128) >> 8);
      dst[4 * x + 3] = 255;
    }
  }
  return 0;
}

}  // namespace libyuv

#endif  // ARC_WASM_SHIM_LIBYUV_H_
