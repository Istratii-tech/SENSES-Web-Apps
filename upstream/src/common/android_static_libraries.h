// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The names of statically linked Android libraries.
//

#ifndef COMMON_ANDROID_STATIC_LIBRARIES_H_
#define COMMON_ANDROID_STATIC_LIBRARIES_H_

namespace arc {

// This array contains the name of statically linked Android libraries
// such as libbinder and libpng. Note they do not have ".a"
// suffix. This list is terminated by a NULL.
extern const char* kAndroidStaticLibraries[];

}  // namespace arc

#endif  // COMMON_ANDROID_STATIC_LIBRARIES_H_
