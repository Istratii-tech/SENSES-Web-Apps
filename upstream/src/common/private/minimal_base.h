// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_PRIVATE_MINIMAL_BASE_H_
#define COMMON_PRIVATE_MINIMAL_BASE_H_

// A macro to disallow the copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define COMMON_DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);                      \
  void operator=(const TypeName&)

#endif  // COMMON_PRIVATE_MINIMAL_BASE_H_
