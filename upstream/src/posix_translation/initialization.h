// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_INITIALIZATION_H_
#define POSIX_TRANSLATION_INITIALIZATION_H_

namespace posix_translation {

// Initializes POSIX translation by installing IRT hooks etc.
// This function must be called before any binary linked with -Wl,--wrap
// calls wrapped functions.
void Initialize();

// Initializes POSIX translation for posix_translation_test.
// Some tests in posix_translation_test call real_XXX functions. To make them
// work, this function sets up the *_real pointers.
// This function is only for posix_translation_test and never ARC_EXPORT'ed.
void InitializeForPosixTranslationTest();

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_INITIALIZATION_H_
