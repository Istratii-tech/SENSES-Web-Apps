// See yuv.h. Integer arithmetic of the usual BT.601/709/2020 matrices (8-bit,
// << 8 fixed point). Part of the arc-wasm shim layer (topic mediacodec).

#include "yuv.h"

#include <string.h>

namespace arcwasm_media {
namespace {

inline uint8_t Clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : static_cast<uint8_t>(v); }

// Full range to video range: Y 0..255 -> 16..235, C 0..255 -> 16..240.
inline uint8_t LimitY(int y) { return static_cast<uint8_t>(16 + ((y * 219 + 127) / 255)); }
inline uint8_t LimitC(int c) { return static_cast<uint8_t>(128 + (((c - 128) * 224) / 255)); }

void CopyPlane(const uint8_t* src, int src_stride, uint8_t* dst, int dst_stride, int w, int h,
               bool limit_y, bool limit_c) {
  for (int y = 0; y < h; y++) {
    const uint8_t* s = src + static_cast<size_t>(y) * src_stride;
    uint8_t* d = dst + static_cast<size_t>(y) * dst_stride;
    if (limit_y) {
      for (int x = 0; x < w; x++)
        d[x] = LimitY(s[x]);
    } else if (limit_c) {
      for (int x = 0; x < w; x++)
        d[x] = LimitC(s[x]);
    } else {
      memcpy(d, s, w);
    }
  }
}

// Pads the right and bottom of a plane (the output may be larger than the
// frame) by repeating the last column and row.
void PadPlane(uint8_t* p, int stride, int w, int h, int fw, int fh) {
  if (fw <= 0 || fh <= 0)
    return;
  for (int y = 0; y < fh; y++) {
    uint8_t* row = p + static_cast<size_t>(y) * stride;
    for (int x = fw; x < w; x++)
      row[x] = row[fw - 1];
  }
  for (int y = fh; y < h; y++)
    memcpy(p + static_cast<size_t>(y) * stride, p + static_cast<size_t>(fh - 1) * stride, w);
}

}  // namespace

void ToI420(const FrameInfo& info, const uint8_t* src, uint8_t* dst, int w, int h,
            bool full_to_limited) {
  const int fw = info.width < w ? info.width : w;
  const int fh = info.height < h ? info.height : h;
  uint8_t* Y = dst;
  uint8_t* U = dst + static_cast<size_t>(w) * h;
  uint8_t* V = U + static_cast<size_t>(w / 2) * (h / 2);
  const int cw = (fw + 1) / 2, ch = (fh + 1) / 2;
  const bool lim = full_to_limited;
  switch (info.format) {
    case kFrameI420:
    case kFrameI420A:
      CopyPlane(src + info.plane[0].offset, info.plane[0].stride, Y, w, fw, fh, lim, false);
      CopyPlane(src + info.plane[1].offset, info.plane[1].stride, U, w / 2, cw, ch, false, lim);
      CopyPlane(src + info.plane[2].offset, info.plane[2].stride, V, w / 2, cw, ch, false, lim);
      break;
    case kFrameI422:
    case kFrameI444: {
      CopyPlane(src + info.plane[0].offset, info.plane[0].stride, Y, w, fw, fh, lim, false);
      const int xs = info.format == kFrameI444 ? 2 : 1;  // chroma columns per output sample
      for (int p = 1; p <= 2; p++) {
        uint8_t* out = p == 1 ? U : V;
        const uint8_t* in = src + info.plane[p].offset;
        for (int y = 0; y < ch; y++) {
          const uint8_t* r0 = in + static_cast<size_t>(2 * y) * info.plane[p].stride;
          const uint8_t* r1 = (2 * y + 1 < fh) ? r0 + info.plane[p].stride : r0;
          for (int x = 0; x < cw; x++) {
            int v = (r0[x * xs] + r1[x * xs] + 1) >> 1;
            out[static_cast<size_t>(y) * (w / 2) + x] = lim ? LimitC(v) : v;
          }
        }
      }
      break;
    }
    case kFrameNV12: {
      CopyPlane(src + info.plane[0].offset, info.plane[0].stride, Y, w, fw, fh, lim, false);
      const uint8_t* uv = src + info.plane[1].offset;
      for (int y = 0; y < ch; y++) {
        const uint8_t* s = uv + static_cast<size_t>(y) * info.plane[1].stride;
        uint8_t* u = U + static_cast<size_t>(y) * (w / 2);
        uint8_t* v = V + static_cast<size_t>(y) * (w / 2);
        if (lim) {
          for (int x = 0; x < cw; x++) {
            u[x] = LimitC(s[2 * x]);
            v[x] = LimitC(s[2 * x + 1]);
          }
        } else {
          for (int x = 0; x < cw; x++) {
            u[x] = s[2 * x];
            v[x] = s[2 * x + 1];
          }
        }
      }
      break;
    }
    case kFrameRGBA:
    case kFrameRGBX:
    case kFrameBGRA:
    case kFrameBGRX: {
      // BT.601 video range, as a decoder of an SD stream would give.
      const bool bgr = info.format == kFrameBGRA || info.format == kFrameBGRX;
      const uint8_t* rgb = src + info.plane[0].offset;
      const int stride = info.plane[0].stride;
      for (int y = 0; y < fh; y++) {
        const uint8_t* s = rgb + static_cast<size_t>(y) * stride;
        uint8_t* d = Y + static_cast<size_t>(y) * w;
        for (int x = 0; x < fw; x++) {
          int r = s[4 * x + (bgr ? 2 : 0)], g = s[4 * x + 1], b = s[4 * x + (bgr ? 0 : 2)];
          d[x] = Clamp(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
        }
      }
      for (int y = 0; y < ch; y++) {
        const uint8_t* s0 = rgb + static_cast<size_t>(2 * y) * stride;
        const uint8_t* s1 = (2 * y + 1 < fh) ? s0 + stride : s0;
        for (int x = 0; x < cw; x++) {
          int x1 = (2 * x + 1 < fw) ? 2 * x + 1 : 2 * x;
          int r = 0, g = 0, b = 0;
          const uint8_t* px[4] = {s0 + 8 * x, s0 + 4 * x1, s1 + 8 * x, s1 + 4 * x1};
          for (const uint8_t* q : px) {
            r += q[bgr ? 2 : 0];
            g += q[1];
            b += q[bgr ? 0 : 2];
          }
          r = (r + 2) >> 2, g = (g + 2) >> 2, b = (b + 2) >> 2;
          U[static_cast<size_t>(y) * (w / 2) + x] = Clamp(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
          V[static_cast<size_t>(y) * (w / 2) + x] = Clamp(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
        }
      }
      break;
    }
    default:
      memset(Y, 16, static_cast<size_t>(w) * h);
      memset(U, 128, static_cast<size_t>(w / 2) * (h / 2) * 2);
      return;
  }
  PadPlane(Y, w, w, h, fw, fh);
  PadPlane(U, w / 2, w / 2, h / 2, cw, ch);
  PadPlane(V, w / 2, w / 2, h / 2, cw, ch);
}

void I420ToRgbx(const uint8_t* src, int w, int h, int cx, int cy, int cw, int chh, int matrix,
                bool full_range, uint32_t* dst, int dst_stride, int dw, int dh) {
  if (cw <= 0 || chh <= 0 || dw <= 0 || dh <= 0)
    return;
  const uint8_t* Y = src;
  const uint8_t* U = src + static_cast<size_t>(w) * h;
  const uint8_t* V = U + static_cast<size_t>(w / 2) * (h / 2);
  // R = ky*Y' + rv*V', G = ky*Y' - gu*U' - gv*V', B = ky*Y' + bu*U' (<< 8)
  int ky, rv, gu, gv, bu, yoff;
  if (full_range) {
    ky = 256, yoff = 0;
    if (matrix == kMatrix709) rv = 403, gu = 48, gv = 120, bu = 475;
    else if (matrix == kMatrix2020) rv = 377, gu = 42, gv = 146, bu = 482;
    else rv = 359, gu = 88, gv = 183, bu = 454;
  } else {
    ky = 298, yoff = 16;
    if (matrix == kMatrix709) rv = 459, gu = 55, gv = 136, bu = 541;
    else if (matrix == kMatrix2020) rv = 430, gu = 48, gv = 167, bu = 548;
    else rv = 409, gu = 100, gv = 208, bu = 516;
  }
  // Source position of each destination pixel (16.16 fixed point, nearest).
  const uint32_t step_x = (static_cast<uint32_t>(cw) << 16) / dw;
  const uint32_t step_y = (static_cast<uint32_t>(chh) << 16) / dh;
  for (int y = 0; y < dh; y++) {
    int sy = cy + static_cast<int>((y * step_y + step_y / 2) >> 16);
    if (sy >= h) sy = h - 1;
    const uint8_t* yr = Y + static_cast<size_t>(sy) * w;
    const uint8_t* ur = U + static_cast<size_t>(sy / 2) * (w / 2);
    const uint8_t* vr = V + static_cast<size_t>(sy / 2) * (w / 2);
    uint32_t* d = dst + static_cast<size_t>(y) * dst_stride;
    uint32_t fx = step_x / 2;
    for (int x = 0; x < dw; x++, fx += step_x) {
      int sx = cx + static_cast<int>(fx >> 16);
      if (sx >= w) sx = w - 1;
      int c = (yr[sx] - yoff) * ky;
      int du = ur[sx >> 1] - 128, dv = vr[sx >> 1] - 128;
      int r = Clamp((c + rv * dv + 128) >> 8);
      int g = Clamp((c - gu * du - gv * dv + 128) >> 8);
      int b = Clamp((c + bu * du + 128) >> 8);
      d[x] = 0xff000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
    }
  }
}

}  // namespace arcwasm_media
