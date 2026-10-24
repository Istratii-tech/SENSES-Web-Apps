// Minimal stand-in for chromium base/memory/scoped_vector.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_MEMORY_SCOPED_VECTOR_H_
#define ARC_WASM_SHIM_BASE_MEMORY_SCOPED_VECTOR_H_

#include <stddef.h>

#include <utility>
#include <vector>

#include "base/macros.h"

// A vector that owns (deletes) the pointers it holds.
template <class T>
class ScopedVector {
 public:
  typedef typename std::vector<T*>::allocator_type allocator_type;
  typedef typename std::vector<T*>::size_type size_type;
  typedef typename std::vector<T*>::difference_type difference_type;
  typedef typename std::vector<T*>::pointer pointer;
  typedef typename std::vector<T*>::const_pointer const_pointer;
  typedef typename std::vector<T*>::reference reference;
  typedef typename std::vector<T*>::const_reference const_reference;
  typedef typename std::vector<T*>::value_type value_type;
  typedef typename std::vector<T*>::iterator iterator;
  typedef typename std::vector<T*>::const_iterator const_iterator;
  typedef typename std::vector<T*>::reverse_iterator reverse_iterator;
  typedef typename std::vector<T*>::const_reverse_iterator
      const_reverse_iterator;

  ScopedVector() {}
  ~ScopedVector() { clear(); }
  ScopedVector(ScopedVector&& other) { swap(other); }
  ScopedVector& operator=(ScopedVector&& rhs) {
    swap(rhs);
    return *this;
  }
  ScopedVector Pass() { return ScopedVector(std::move(*this)); }

  reference operator[](size_t index) { return v_[index]; }
  const_reference operator[](size_t index) const { return v_[index]; }

  bool empty() const { return v_.empty(); }
  size_t size() const { return v_.size(); }

  reverse_iterator rbegin() { return v_.rbegin(); }
  const_reverse_iterator rbegin() const { return v_.rbegin(); }
  reverse_iterator rend() { return v_.rend(); }
  const_reverse_iterator rend() const { return v_.rend(); }

  iterator begin() { return v_.begin(); }
  const_iterator begin() const { return v_.begin(); }
  iterator end() { return v_.end(); }
  const_iterator end() const { return v_.end(); }

  const_reference front() const { return v_.front(); }
  reference front() { return v_.front(); }
  const_reference back() const { return v_.back(); }
  reference back() { return v_.back(); }

  void push_back(T* elem) { v_.push_back(elem); }
  void pop_back() {
    delete v_.back();
    v_.pop_back();
  }

  std::vector<T*>& get() { return v_; }
  const std::vector<T*>& get() const { return v_; }
  void swap(std::vector<T*>& other) { v_.swap(other); }
  void swap(ScopedVector<T>& other) { v_.swap(other.v_); }
  void release(std::vector<T*>* out) {
    out->swap(v_);
    v_.clear();
  }

  void reserve(size_t capacity) { v_.reserve(capacity); }

  // Resize, deleting elements in the disappearing range if we are shrinking.
  void resize(size_t new_size) {
    for (size_t i = new_size; i < v_.size(); ++i)
      delete v_[i];
    v_.resize(new_size);
  }

  void clear() {
    for (size_t i = 0; i < v_.size(); ++i)
      delete v_[i];
    v_.clear();
  }
  void weak_clear() { v_.clear(); }

  iterator insert(iterator position, T* x) { return v_.insert(position, x); }
  iterator erase(iterator position) {
    delete *position;
    return v_.erase(position);
  }
  iterator weak_erase(iterator position) { return v_.erase(position); }

 private:
  std::vector<T*> v_;
  DISALLOW_COPY_AND_ASSIGN(ScopedVector);
};

#endif  // ARC_WASM_SHIM_BASE_MEMORY_SCOPED_VECTOR_H_
