/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GRAPHICS_TRANSLATION_EGL_OBJECT_REGISTRY_H_
#define GRAPHICS_TRANSLATION_EGL_OBJECT_REGISTRY_H_

#include <unistd.h>
#include <map>
#include <vector>
#include "graphics_translation/gles/mutex.h"

// A simple class used to "own" objects whose lifetimes are managed by the
// android::sp class.
template<typename Object>
class ObjectRegistry {
 public:
  typedef void* Key;

  ObjectRegistry() : key_gen_(0) {
  }

  Key GenerateKey() {
    Key key = 0;
    do {
      key = reinterpret_cast<Key>(++key_gen_);
    } while (key == 0 || objects_.find(key) != objects_.end());
    return key;
  }

  void Acquire() {
    Mutex::Autolock mutex(&lock_);
    const pid_t pid = getpid();
    int& count = counts_[pid];
    ++count;
  }

  void Release() {
    Mutex::Autolock mutex(&lock_);
    const pid_t pid = getpid();
    int& count = counts_[pid];
    --count;
    if (count == 0) {
      typename Objects::iterator iter = objects_.begin();
      while (iter != objects_.end()) {
        if (iter->second.process_ == pid) {
          objects_.erase(iter++);
        } else {
          ++iter;
        }
      }
    } else if (count < 0) {
      count = 0;
    }
  }

  Key Register(const Object& obj) {
    Mutex::Autolock mutex(&lock_);
    const Key key = obj->GetKey();
    objects_[key] = Entry(obj);
    return key;
  }

  void Unregister(Key key) {
    Mutex::Autolock mutex(&lock_);
    const typename Objects::iterator iter = objects_.find(key);
    if (iter != objects_.end()) {
      objects_.erase(iter);
    }
  }

  Object Get(Key key) {
    Mutex::Autolock mutex(&lock_);
    const typename Objects::iterator iter = objects_.find(key);
    return iter != objects_.end() ? iter->second.object_ : Object();
  }

  typedef std::vector<Object> ObjectList;
  ObjectList GetAllObjects() {
    Mutex::Autolock mutex(&lock_);
    ObjectList objects;
    for (typename Objects::const_iterator iter = objects_.begin();
        iter != objects_.end(); ++iter) {
      objects.push_back(iter->second.object_);
    }
    return objects;
  }

 private:
  struct Entry {
    Entry() : process_(0), object_() {}
    explicit Entry(Object o) : process_(getpid()), object_(o) {}
    pid_t process_;
    Object object_;
  };

  typedef std::map<Key, Entry> Objects;
  typedef std::map<pid_t, int> RefCounts;
  Mutex lock_;
  uint32_t key_gen_;
  RefCounts counts_;
  Objects objects_;

  ObjectRegistry(const ObjectRegistry&);
  ObjectRegistry& operator=(const ObjectRegistry&);
};


#endif  // GRAPHICS_TRANSLATION_EGL_OBJECT_REGISTRY_H_
