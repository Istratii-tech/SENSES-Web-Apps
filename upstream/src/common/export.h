// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_EXPORT_H_
#define COMMON_EXPORT_H_

// Exports a class or function from a DSO that is compiled with
// -fvisibility=hidden. Note that you do not have to use this macro
// for the main executable.
#define ARC_EXPORT __attribute__((__visibility__("default")))

#endif  // COMMON_EXPORT_H_
