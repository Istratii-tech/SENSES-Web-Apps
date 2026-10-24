// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Takes an image bitmap as input and prints it out in ANSI format.

#include <common/print_image.h>
#include <stdio.h>

template <class T> struct ImageBits {
  T r, g, b, a;
};

// We pick 5x10 pixels to be expressed per pixel.  We could have
// instead picked a number of characters max to display the image, but
// in debugging it was often useful to see the relative sizes of
// different images.
const int kCharPixelWidth = 5;
const int kCharPixelHeight = 10;

// Threshold value for an 8-bit color channel signal level to be shown
// in output.
const unsigned char kColorThreshold = 128;

namespace arc {

void PrintImage(FILE* fp, void* data_rgba8, int width, int height,
                bool upside_down) {
  if (width <= 0 || height <= 0) return;
  fprintf(fp, "\e[0m");
  const int kAnsiRed = 1;
  const int kAnsiGreen = 2;
  const int kAnsiBlue = 4;
  ImageBits<unsigned char>* texture =
    reinterpret_cast<ImageBits<unsigned char>*>(data_rgba8);
  for (int sy = 0; sy < height; sy += kCharPixelHeight) {
    for (int sx = 0; sx < width; sx += kCharPixelWidth) {
      ImageBits<uint64_t> summed = {};
      int samples = 0;
      for (int suby = sy; suby < sy + kCharPixelHeight; ++suby) {
        if (suby >= height)
          break;
        for (int subx = sx; subx < sx + kCharPixelHeight; ++subx) {
          if (subx >= width)
            break;
          ImageBits<unsigned char>* here = NULL;
          if (upside_down)
            here = &texture[(height - 1 - suby) * width + subx];
          else
            here = &texture[suby * width + subx];
          summed.r += here->r;
          summed.g += here->g;
          summed.b += here->b;
          summed.a += here->a;
          ++samples;
        }
      }
      summed.r /= samples;
      summed.g /= samples;
      summed.b /= samples;
      summed.a /= samples;
      int ansi_color = 0;
      if (summed.r > kColorThreshold) ansi_color |= kAnsiRed;
      if (summed.g > kColorThreshold) ansi_color |= kAnsiGreen;
      if (summed.b > kColorThreshold) ansi_color |= kAnsiBlue;
      fprintf(fp, "\e[4%dm%s", ansi_color,
              summed.a > kColorThreshold ? " " : ".");
    }
    fprintf(fp, "\e[0m");
    fprintf(fp, "\n");
  }
}

}  // namespace arc
