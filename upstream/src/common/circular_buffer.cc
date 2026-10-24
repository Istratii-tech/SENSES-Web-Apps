// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include "common/circular_buffer.h"

#include <algorithm>

#include "common/alog.h"

namespace arc {

CircularBuffer::CircularBuffer() : buffer_(NULL), start_(0),
    end_(0), capacity_(0), size_(0) {}

CircularBuffer::~CircularBuffer() {
  delete[] buffer_;
}

void CircularBuffer::clear() {
  start_ = 0;
  end_ = 0;
  size_ = 0;
}

void CircularBuffer::set_capacity(size_t capacity) {
  if (capacity < size_)
    ALOGW("Truncating circular buffer will result in loss of data");
  size_t new_size = std::min(size_, capacity);
  if (new_size == 0) {
    // As there is no data to copy, we will first delete the old buffer so we
    // minimize peak memory consumption.
    delete[] buffer_;
    buffer_ = NULL;
  }
  char* new_buffer = new char[capacity];
  ALOG_ASSERT(new_buffer != NULL);
  if (new_size > 0) {
    ALOG_ASSERT(buffer_ != NULL);
    read(new_buffer, new_size);
  }

  delete[] buffer_;
  start_ = 0;
  end_ = new_size;
  size_ = new_size;
  buffer_ = new_buffer;
  capacity_ = capacity;
}

size_t CircularBuffer::write(const char* buf, size_t len) {
  len = std::min(len, capacity_ - size_);
  if (len <= capacity_ - end_) {
    memcpy(buffer_ + end_, buf, len);
    end_ += len;
  } else {
    size_t end_size = capacity_ - end_;
    size_t front_size = len - end_size;
    memcpy(buffer_ + end_, buf, end_size);
    memcpy(buffer_, buf + end_size, front_size);
    end_ = front_size;
  }
  size_ += len;
  ALOG_ASSERT(size_ <= capacity_);
  ALOG_ASSERT(len <= capacity_);
  return len;
}

size_t CircularBuffer::read(char* buf, size_t len) {
  size_t end_size = capacity_ - start_;
  len = std::min(len, size_);
  if (len == 0)
    return len;
  ALOG_ASSERT(buffer_ != NULL);
  if (len <= end_size) {
    memcpy(buf, buffer_ + start_, len);
    start_ += len;
    if (start_ == capacity_)
      start_ = 0;
  } else {
    size_t start_size = len - end_size;
    memcpy(buf, buffer_ + start_, end_size);
    memcpy(buf + end_size, buffer_, start_size);
    start_ = start_size;
  }
  size_ -= len;
  ALOG_ASSERT(size_ <= capacity_);
  ALOG_ASSERT(len <= capacity_);
  return len;
}

}  // namespace arc
