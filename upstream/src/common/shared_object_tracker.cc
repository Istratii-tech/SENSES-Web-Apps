// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Tracks lifetimes of objects created by other components.

#include "common/shared_object_tracker.h"

#include <map>

#include "base/memory/singleton.h"
#include "base/synchronization/lock.h"
#include "common/alog.h"

namespace arc {

class TrackerStorage {
 public:
  static TrackerStorage* GetInstance();

  int Register(SharedObjectDestroyFunc destroy_func, void* param);
  void IncRef(int handle);
  bool DecRef(int handle);

 private:
  struct Object {
    Object(SharedObjectDestroyFunc destroy_func, void* param);
    ~Object();

    void IncRef();
    bool DecRef();

   private:
    Object(const Object&);
    void operator=(const Object&);

    SharedObjectDestroyFunc destroy_func_;
    void* param_;
    int ref_count_;
  };

  typedef std::map<int, Object*> ObjectMap;

  TrackerStorage() : handle_gen_(0) {}
  ~TrackerStorage() {}

  Object* FindObjectLocked(int handle);

  base::Lock mu_;
  ObjectMap objects_;
  int handle_gen_;

  friend struct DefaultSingletonTraits<TrackerStorage>;
};

TrackerStorage::Object::Object(
    SharedObjectDestroyFunc destroy_func, void* param)
    : destroy_func_(destroy_func), param_(param), ref_count_(1) {
  ALOG_ASSERT(destroy_func);
}

TrackerStorage::Object::~Object() {
  destroy_func_(param_);
}

void TrackerStorage::Object::IncRef() {
  ref_count_++;
}

bool TrackerStorage::Object::DecRef() {
  ref_count_--;
  return (ref_count_ == 0);
}

TrackerStorage* TrackerStorage::GetInstance() {
  return Singleton<TrackerStorage,
      LeakySingletonTraits<TrackerStorage> >::get();
}

int TrackerStorage::Register(
    SharedObjectDestroyFunc destroy_func, void* param) {
  base::AutoLock lock(mu_);
  LOG_ALWAYS_FATAL_IF(handle_gen_ == INT_MAX);
  int handle = ++handle_gen_;
  Object* obj = new Object(destroy_func, param);
  objects_[handle] = obj;
  return handle;
}

void TrackerStorage::IncRef(int handle) {
  base::AutoLock lock(mu_);
  Object* obj = FindObjectLocked(handle);
  if (!obj) {
    ALOG_ASSERT(false);
    return;
  }
  obj->IncRef();
}

bool TrackerStorage::DecRef(int handle) {
  if (!handle)
    return false;

  bool is_dead;
  Object* obj;

  {
    base::AutoLock lock(mu_);
    obj = FindObjectLocked(handle);
    if (!obj) {
      ALOG_ASSERT(false);
      return false;
    }
    is_dead = obj->DecRef();
  }

  if (is_dead) {
    objects_.erase(handle);
    delete obj;
  }

  return is_dead;
}

TrackerStorage::Object* TrackerStorage::FindObjectLocked(int handle) {
  ObjectMap::iterator it = objects_.find(handle);
  return (it != objects_.end() ? it->second : NULL);
}

int SharedObjectTracker::Register(
    SharedObjectDestroyFunc destroy_func, void* param) {
  return TrackerStorage::GetInstance()->Register(destroy_func, param);
}

void SharedObjectTracker::IncRef(int handle) {
  TrackerStorage::GetInstance()->IncRef(handle);
}

bool SharedObjectTracker::DecRef(int handle) {
  return TrackerStorage::GetInstance()->DecRef(handle);
}

}  // namespace arc
