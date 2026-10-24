#!src/build/run_python
#
# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generates wrap_syscall_aliases.S.

The generated source code defines aliases from __wrap_FUNC to FUNC.
It is linked to pseudo libposix_translation.so for testing so that unit tests
can load any DSOs that depend on libposix_translation.so, and refer
posix functions that are renamed to __wrap_FUNC by --wrap.
"""

import sys

from src.build import wrapped_functions
from src.build.build_options import OPTIONS


def main():
  OPTIONS.parse_configure_file()
  print '''// Auto-generated file - DO NOT EDIT!
// THIS FILE SHOULD BE USED FOR UNIT TESTS ONLY.

#if defined(__native_client__) && defined(__i386__)
.type get_pc_thunk_cx, function
get_pc_thunk_cx:
    popl %ecx
    nacljmp %ecx
#endif

.macro trampoline_to_original_libc_call function
#if defined(__native_client__)
    #if defined(__i386__)
    call get_pc_thunk_cx
    addl $_GLOBAL_OFFSET_TABLE_, %ecx
    movl \\function@GOT(%ecx), %ecx
    nacljmp %ecx
    #elif defined(__x86_64__)
    jmp \\function@PLT
    #else
    #error "Unsupported NaCl architecture"
    #endif
#else  // defined(__native_client__)
    #if defined(__i386__)
    jmp \\function
    #elif defined(__arm__)
    b \\function
    #else
    #error "Unsupported architecture"
    #endif
#endif
.endm

.macro define_wrap_function_to_call_original_function function
.globl __wrap_\\function
.type __wrap_\\function, function
#if defined(__native_client__)
.balign 32
#endif  // defined(__native_client__)
__wrap_\\function:
    trampoline_to_original_libc_call \\function
.endm

'''

  for name in wrapped_functions.get_wrapped_functions():
    print 'define_wrap_function_to_call_original_function %s' % name


if __name__ == '__main__':
  sys.exit(main())
