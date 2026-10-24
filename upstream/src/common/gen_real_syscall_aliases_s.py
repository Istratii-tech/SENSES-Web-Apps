#!src/build/run_python
#
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generates real_syscall_aliases.S.

The generated source code defines aliases from __real_FUNC to FUNC.
It is linked to unit tests that call __real_FUNC but are not linked with
--wrap.
"""

import sys

from src.build import wrapped_functions
from src.build.build_options import OPTIONS


def main():
  OPTIONS.parse_configure_file()

  print '// Auto-generated file - DO NOT EDIT!'
  print '// THIS FILE SHOULD BE USED FOR UNIT TESTS ONLY.'
  print

  for name in wrapped_functions.get_wrapped_functions():
    print '.globl __real_%s' % name
    print '.type __real_%s, function' % name

  print '#if defined(__x86_64__) || defined(__i386__)'
  for name in wrapped_functions.get_wrapped_functions():
    print '__real_%s: jmp %s@PLT' % (name, name)

  print '#elif defined(__arm__)'
  for name in wrapped_functions.get_wrapped_functions():
    print '__real_%s: b %s@PLT' % (name, name)

  print '#else'
  print '#error "Unsupported architecture"'
  print '#endif'


if __name__ == '__main__':
  sys.exit(main())
