// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Tracks lifetimes of objects created by other components.

#ifndef COMMON_SHARED_OBJECT_TRACKER_H_
#define COMMON_SHARED_OBJECT_TRACKER_H_

namespace arc {

typedef void (*SharedObjectDestroyFunc)(void* param);

// Tracks lifetimes of objects created by other components.
// An example is video decoder that has to remain alive until all of its
// graphics buffers have been freed.
class SharedObjectTracker {
 public:
  // Registers an object with a ref count of 1. Returns non-zero handle.
  static int Register(SharedObjectDestroyFunc destroy_func, void* param);

  // Increments ref count for the given handle.
  static void IncRef(int handle);

  // Decrements ref count for the given handle, invokes destroy_func
  // and returns true when ref count reaches zero. Zero handle is ignored.
  static bool DecRef(int handle);

 private:
  SharedObjectTracker();
  ~SharedObjectTracker();
};

}  // namespace arc

#endif  // COMMON_SHARED_OBJECT_TRACKER_H_
