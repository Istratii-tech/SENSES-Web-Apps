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
#ifndef GRAPHICS_TRANSLATION_GLES_COND_H_
#define GRAPHICS_TRANSLATION_GLES_COND_H_

#include <pthread.h>
#include "mutex.h"

class Cond {
 public:
  Cond() {
    pthread_cond_init(&cond_, NULL);
  }

  ~Cond() {
    pthread_cond_destroy(&cond_);
  }

  void Wait(Mutex& mutex) {
    pthread_cond_wait(&cond_, &mutex.GetUnderlyingMutex());
  }

  void Signal() {
    pthread_cond_signal(&cond_);
  }

 private:
  pthread_cond_t cond_;

  Cond(const Cond& rhs);
  Cond& operator=(const Cond& rhs);
};

#endif  // GRAPHICS_TRANSLATION_GLES_COND_H_
