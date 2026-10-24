# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from src.build import ninja_generator


def generate_ninjas():
  n = ninja_generator.SharedObjectNinjaGenerator(
      'hwcomposer.default', install_path='/lib/hw',
      force_compiler='clang',
      enable_cxx11=True,
      base_path='android/hardware/arc/hwcomposer')

  # hwcomposer.cpp uses gcc-style struct initialization "member: value"
  # which clang warns by default. We cannot switch to the C++11 style
  # ".member = value" yet as some of our compilers do not support it.
  # TODO(crbug.com/365178): Switch to ".member = value" style and remove
  # -Wno-gnu-designator.
  n.add_compiler_flags('-Werror', '-Wno-gnu-designator')
  n.emit_framework_common_flags()
  n.add_compiler_flags('-Wno-unused-variable', '-Wno-unused-function',
                       '-Werror')
  n.add_notice_sources(['mods/graphics_translation/NOTICE'])
  n.add_include_paths('mods/graphics_translation')
  sources = ['graphics_translation/hwcomposer/hwcomposer.cpp']
  n.build_default(sources, base_path='mods')
  n.add_library_deps('libcutils.so')
  n.link()
