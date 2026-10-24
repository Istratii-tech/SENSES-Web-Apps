// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_REAL_SYSCALL_H_
#define POSIX_TRANSLATION_REAL_SYSCALL_H_

// These functions call into the original IRT bypassing the hooks. They are not
// ARC_EXPORT'ed and are for internal use.
extern "C" {
int real_close(int fd);
int real_fstat(int fd, struct stat* buf);
off64_t real_lseek64(int fd, off64_t offset, int whence);
ssize_t real_read(int fd, void* buf, size_t count);
ssize_t real_write(int fd, const void* buf, size_t count);
}  // extern "C"

#endif  // POSIX_TRANSLATION_REAL_SYSCALL_H_
