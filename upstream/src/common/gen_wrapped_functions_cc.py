#!src/build/run_python
#
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generate wrapped_functions.cc, which contains the list of wrapped functions.
"""

import string
import sys

from src.build import wrapped_functions
from src.build.build_options import OPTIONS


_WRAPPED_FUNCTIONS_CC_TEMPLATE = string.Template("""
// Auto-generated file - DO NOT EDIT!

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#include "common/wrapped_functions.h"
#include "common/wrapped_function_declarations.h"

namespace arc {

WrappedFunction kWrappedFunctions[] = {
${WRAPPED_FUNCTIONS}
  { 0, 0 }
};

}  // namespace arc
""")


def main():
  OPTIONS.parse_configure_file()

  functions = []
  for function in wrapped_functions.get_wrapped_functions():
    functions.append('  { "%s", reinterpret_cast<void*>(%s) },' %
                     (function, function))
  sys.stdout.write(_WRAPPED_FUNCTIONS_CC_TEMPLATE.substitute({
      'WRAPPED_FUNCTIONS': '\n'.join(functions)
  }))


if __name__ == '__main__':
  sys.exit(main())
