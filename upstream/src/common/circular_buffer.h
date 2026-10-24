// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_CIRCULAR_BUFFER_H_
#define COMMON_CIRCULAR_BUFFER_H_

#include <stddef.h>

#include "common/private/minimal_base.h"

namespace arc {

// Simple circular buffer class. Client is responsible for thread-safety and
// blocking/retries if reading an empty buffer or writing would overflow the
// buffer.
class CircularBuffer {
 public:
  CircularBuffer();
  ~CircularBuffer();

  size_t capacity() const { return capacity_; }
  void clear();
  size_t read(char* buf, size_t len);
  size_t remaining() const { return capacity_ - size_; }
  void set_capacity(size_t capacity);
  size_t size() const { return size_; }
  size_t write(const char* buf, size_t len);

 private:
  char* buffer_;

  // The beginning of the data currently in the buffer. As this is a circular
  // buffer, |start_| is not necessarily less than or equal to |end_|.
  size_t start_;

  // The end of the data currently in the buffer.
  size_t end_;

  // Total capacity of the buffer.
  size_t capacity_;

  // Current size of data within buffer.
  size_t size_;

  COMMON_DISALLOW_COPY_AND_ASSIGN(CircularBuffer);
};

}  // namespace arc

#endif  // COMMON_CIRCULAR_BUFFER_H_
