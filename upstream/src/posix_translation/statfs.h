// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Functions which fill fixed statfs values got from a real device.

#ifndef POSIX_TRANSLATION_STATFS_H_
#define POSIX_TRANSLATION_STATFS_H_

#include <sys/vfs.h>

namespace posix_translation {

int DoStatFsForDev(struct statfs* out);
int DoStatFsForProc(struct statfs* out);
int DoStatFsForData(struct statfs* out);
int DoStatFsForSystem(struct statfs* out);
int DoStatFsForSys(struct statfs* out);

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_STATFS_H_
