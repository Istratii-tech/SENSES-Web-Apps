// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Takes a texture as input and prints it out in ANSI format.

#ifndef COMMON_PRINT_IMAGE_H_
#define COMMON_PRINT_IMAGE_H_

#include <stdio.h>

namespace arc {

void PrintImage(FILE* fp, void* data_rgba8, int twidth, int theight,
                bool upside_down);

}  // namespace arc

#endif  // COMMON_PRINT_IMAGE_H_
