/*
 * Copyright (C) 2011 The Android Open Source Project
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
#ifndef GRAPHICS_TRANSLATION_GLES_MUTEX_H_
#define GRAPHICS_TRANSLATION_GLES_MUTEX_H_

#include <pthread.h>

class Mutex {
 public:
  Mutex() {
    pthread_mutex_init(&mutex_, NULL);
  }

  ~Mutex() {
    pthread_mutex_destroy(&mutex_);
  }

  void Lock() {
    pthread_mutex_lock(&mutex_);
  }

  void Unlock() {
    pthread_mutex_unlock(&mutex_);
  }

  class Autolock {
   public:
    explicit Autolock(Mutex* m) : mutex_(m) {
      mutex_->Lock();
    }

    ~Autolock() {
      mutex_->Unlock();
    }

   private:
    Mutex* mutex_;

    Autolock(const Autolock& rhs);
    Autolock& operator=(const Autolock& rhs);
  };

 private:
  friend class Cond;

  pthread_mutex_t mutex_;

  pthread_mutex_t& GetUnderlyingMutex() {
      return mutex_;
  }

  Mutex(const Mutex& rhs);
  Mutex& operator=(const Mutex& rhs);
};

#endif  // GRAPHICS_TRANSLATION_GLES_MUTEX_H_
