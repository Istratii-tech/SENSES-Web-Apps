// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/statfs.h"

#include <string.h>

// Note: These values are obtained by running a small C program on a
// real device using C4droid.

// See man statfs(2).
#define TMPFS_MAGIC           0x01021994
#define PROC_SUPER_MAGIC      0x9fa0
#define EXT2_SUPER_MAGIC      0xEF53
#define SYSFS_MAGIC           0x62656572

namespace posix_translation {

int DoStatFsForDev(struct statfs* out) {
  memset(out, 0, sizeof(struct statfs));
  out->f_type = TMPFS_MAGIC;
  out->f_bsize = 4096;
  out->f_blocks = 88936;
  out->f_bfree = 88928;
  out->f_bavail = 88928;
  out->f_files = 28368;
  out->f_ffree = 28134;
  out->f_namelen = 255;
  out->f_frsize = 4096;
  out->f_spare[0] = 4130;
  return 0;
}

int DoStatFsForProc(struct statfs* out) {
  memset(out, 0, sizeof(struct statfs));
  out->f_type = PROC_SUPER_MAGIC;
  out->f_bsize = 4096;
  out->f_blocks = 88936;
  out->f_bfree = 88928;
  out->f_bavail = 88928;
  out->f_files = 28368;
  out->f_ffree = 28134;
  out->f_namelen = 255;
  out->f_frsize = 4096;
  out->f_spare[0] = 4128;
  return 0;
}

int DoStatFsForData(struct statfs* out) {
  memset(out, 0, sizeof(struct statfs));
  out->f_type = EXT2_SUPER_MAGIC;
  out->f_bsize = 4096;
  out->f_blocks = 2LL * 1024 * 1024 * 1024 / out->f_bsize;  // 2GB
  out->f_bfree = out->f_blocks / 2;
  out->f_bavail = out->f_bfree;
  out->f_files = 887696;
  out->f_ffree = 866497;
  out->f_fsid.__val[0] = -748642328;
  out->f_fsid.__val[1] = 77008235;
  out->f_namelen = 255;
  out->f_frsize = 4096;
  out->f_spare[0] = 1062;
  return 0;
}

int DoStatFsForSystem(struct statfs* out) {
  memset(out, 0, sizeof(struct statfs));
  out->f_type = EXT2_SUPER_MAGIC;
  out->f_bsize = 4096;
  out->f_blocks = 164788;
  out->f_bfree = 93919;
  out->f_bavail = 93919;
  out->f_files = 41856;
  out->f_ffree = 40924;
  out->f_fsid.__val[0] = -748642328;
  out->f_fsid.__val[1] = 77008235;
  out->f_namelen = 255;
  out->f_frsize = 4096;
  out->f_spare[0] = 4129;
  return 0;
}

int DoStatFsForSys(struct statfs* out) {
  memset(out, 0, sizeof(struct statfs));
  out->f_type = SYSFS_MAGIC;
  out->f_bsize = 4096;
  out->f_blocks = 0;
  out->f_bfree = 0;
  out->f_bavail = 0;
  out->f_files = 0;
  out->f_ffree = 0;
  out->f_fsid.__val[0] = 0;
  out->f_fsid.__val[1] = 0;
  out->f_namelen = 255;
  out->f_frsize = 4096;
  out->f_spare[0] = 4128;
  return 0;
}

}  // namespace posix_translation
